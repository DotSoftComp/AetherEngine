// Aether Engine — visual scripting v2: Blueprint-style gameplay graphs.
//
// A ScriptGraph is nodes + two kinds of links:
//   - EXEC links drive control flow ("when X, do Y then Z"),
//   - DATA links feed typed values (Float/Vec3/Bool/String/Entity) between pins.
// Impure nodes (with exec pins) run when execution reaches them; pure nodes
// (data-only: math, getters, logic) are evaluated on demand when an exec node
// pulls one of their inputs. Graphs support per-instance VARIABLES (declared in
// the asset, Get/Set nodes), FUNCTIONS (Function/Return entries + Call — a
// subroutine call stack on the execution token), and latent flow (Delay parks a
// token; loops via ForLoop or cyclic exec links).
//
// Every node type is SELF-DESCRIBING: registered once in scriptNodeDefs()
// (script_graph.cpp) with its category, typed pins, instance-parameter labels,
// and a single eval lambda. The serializer, the runtime, and the node-canvas
// editor are all table-driven from that registry — adding a new block is one
// small registration and nothing else.
//
// Assets are hand-editable JSON in assets/scripts/ (format v2). The
// ScriptGraph component runs a graph on its entity; entities are referenced
// through Entity-typed values (Self / FindEntity), so one graph asset can
// drive many instances.
#pragma once
#include "../engine/component.h"
#include "../engine/reflect.h"
#include "../core/math3d.h"
#include <functional>
#include <sstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ae {

class World;

// ---- typed values -----------------------------------------------------------
enum class PinType { Exec, Float, Vec3, Bool, String, Entity };
const char* pinTypeName(PinType t);
unsigned pinTypeColor(PinType t); // editor pin/link color (IM_COL32-compatible)

// A runtime value with permissive conversions (Float<->Bool, Float<->Vec3
// splat/x, anything->String) so users rarely fight the type system.
struct Value {
    PinType type = PinType::Float;
    float f = 0.0f;
    Vec3 v{0, 0, 0};
    bool b = false;
    std::string s;
    uint32_t ent = 0; // Entity session id (resolved through the World)

    static Value F(float x) { Value r; r.type = PinType::Float; r.f = x; return r; }
    static Value V(const Vec3& x) { Value r; r.type = PinType::Vec3; r.v = x; return r; }
    static Value B(bool x) { Value r; r.type = PinType::Bool; r.b = x; return r; }
    static Value S(std::string x) { Value r; r.type = PinType::String; r.s = std::move(x); return r; }
    static Value E(uint32_t id) { Value r; r.type = PinType::Entity; r.ent = id; return r; }

    float asF() const;
    Vec3 asV() const;
    bool asB() const;
    std::string asS(const World* w = nullptr) const; // w: entity ids print as names
};

// ---- graph data -------------------------------------------------------------
// A data input is either a link to another node's output or an inline literal.
struct DataIn {
    std::string fromNode; // "" = literal / default
    int fromOut = 0;
    bool hasLiteral = false;
    Value literal;
};

struct ScriptNode {
    std::string id;
    std::string type; // registry key
    std::string p;    // instance string param (var/function name, key, op, path...)
    float n = 0.0f;   // instance number param (radius, flag value...)
    std::vector<std::string> execOut; // exec targets (size = def.execOuts)
    std::vector<DataIn> in;           // data inputs (size = def.dataIns)
    float x = 0.0f, y = 0.0f;         // editor canvas position
};

struct ScriptVariable {
    std::string name;
    Value value; // declared type + default
};

struct ScriptGraph {
    int version = 2;
    std::vector<ScriptVariable> variables;
    std::vector<ScriptNode> nodes;
    int indexOf(const std::string& id) const {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].id == id) return (int)i;
        return -1;
    }
};

bool loadScriptGraph(ScriptGraph& g, const std::string& path);
bool saveScriptGraph(const ScriptGraph& g, const std::string& path);

// Value <-> JSON fragment ("t"/"f"/"v"/"b"/"s" keys), shared with save-games.
struct JsonValue;
void scriptValueWrite(std::ostringstream& o, const Value& v);
Value scriptValueRead(const JsonValue& j);

// ---- the node registry (single source of truth) ------------------------------
class ScriptGraphComponent;
struct NodeCtx; // execution context handed to eval lambdas (defined below)

struct PinDef {
    std::string name;
    PinType type = PinType::Float;
    Value def; // used when a data input is unconnected and has no literal
};

struct NodeDef {
    std::string type;
    std::string category;
    bool isEvent = false;             // fired by the runtime, not by exec links
    std::vector<std::string> execIn;  // 0 = pure node; else 1 entry ("in")
    std::vector<std::string> execOut; // labels ("then", "true"/"false", ...)
    std::vector<PinDef> dataIn;
    std::vector<PinDef> dataOut;
    const char* pLabel = nullptr; // instance param labels (nullptr = unused)
    const char* nLabel = nullptr;
    // Runs the node. Read inputs via ctx, write outputs via ctx, return the
    // exec-out index to follow (kContinue0 for out 0; kEnd to stop the token;
    // kParked after ctx.park()). Pure nodes just fill outputs.
    std::function<int(NodeCtx&)> eval;

    bool pure() const { return execIn.empty() && !isEvent; }
};
static constexpr int kEnd = -1;
static constexpr int kParked = -2;

const std::vector<NodeDef>& scriptNodeDefs();
const NodeDef* scriptNodeDef(const std::string& type);

// ---- execution context --------------------------------------------------------
struct NodeCtx {
    ScriptGraphComponent& comp;
    World& world;
    const ScriptNode& node;
    const NodeDef& def;
    float dt = 0.0f;
    std::vector<Value> outs;          // dataOut slots, filled by eval
    float parkSeconds = -1.0f;        // set by park()
    int depth = 0;                    // pure-eval recursion guard

    Value in(int i);                  // resolve data input i (link/literal/default)
    float inF(int i) { return in(i).asF(); }
    Vec3 inV(int i) { return in(i).asV(); }
    bool inB(int i) { return in(i).asB(); }
    std::string inS(int i);
    Entity* inE(int i);               // resolve Entity value through the World
    void out(int i, const Value& v) { if (i < (int)outs.size()) outs[i] = v; }
    void park(float seconds) { parkSeconds = seconds < 0 ? 0 : seconds; }
    Entity& self();                   // the entity running the graph
};

// ---- runtime component ---------------------------------------------------------
class ScriptGraphComponent : public Component {
public:
    std::string graphPath;

    const char* typeName() const override { return "ScriptGraph"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("graph", graphPath, {PropKind::ScriptGraphPath, "Graph file"});
    }
    void onDeserialized(AssetLibrary& assets) override;
    void onStart() override;
    void onUpdate(float dt) override;

    const ScriptGraph& graph() const { return graph_; }
    void reload(AssetLibrary& assets) { onDeserialized(assets); }

    // Variables (per component instance, reset on reload/start).
    Value getVar(const std::string& name) const;
    void setVar(const std::string& name, const Value& v);
    const std::map<std::string, Value>& vars() const { return vars_; } // save-games

    // Loop counters for ForLoop nodes (keyed per node, per token id).
    std::map<std::pair<int, int>, int> loopState;

    // Pure-node data pull (used by NodeCtx::in through data links).
    Value evalDataOut(int nodeIdx, int outIdx, float dt, int depth);

    std::string resolveAsset(const std::string& rel) const; // for PlaySound etc.
    AssetLibrary* assets() const { return assets_; }        // for GetData etc.

private:
    struct Token {
        int id = 0;
        int node = -1;                 // node to execute next
        float wait = 0.0f;             // parked on a Delay
        int resumeOut = 0;             // exec-out to follow when the wait ends
        std::vector<std::pair<int, int>> callStack; // (returnNode, returnOut) for Call
    };
    void fireEvent(int nodeIndex, float dt, const std::vector<Value>& eventOuts);
    void step(Token& t, float dt);
    friend struct NodeCtx;

    ScriptGraph graph_;
    std::vector<Token> tokens_;
    std::vector<Token> spawnQueue_; // Sequence spawns land here (never mutate
                                    // tokens_ mid-step: iteration holds refs)
    int nextTokenId_ = 1;
    std::map<std::string, Value> vars_;
    std::set<int> onceDone_;
    std::set<int> eventFired_;
    enum class Prox { Unknown, Outside, Inside };
    std::map<int, Prox> prox_;
    std::map<int, bool> keyWas_;
    std::map<int, std::vector<Value>> eventOuts_; // event dataOut values for this fire
    std::string projectRoot_;
    AssetLibrary* assets_ = nullptr; // outlives the World (see AssetLibrary docs)
    bool started_ = false;
};

} // namespace ae
