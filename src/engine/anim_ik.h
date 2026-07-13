// Aether Engine — two-bone inverse kinematics on an animated ModelPose.
//
// Attach next to (or under the same entity as) a Model/Animator. Names an end
// bone (hand, foot); the chain is that bone plus its two parents. Every frame
// after animation and physics (onLateUpdate), the chain is re-aimed so the
// end bone reaches the target entity's position — feet on stairs, hands on
// levers — then blended over the animated pose by `weight`. An optional pole
// entity controls the bend plane (where the knee/elbow points).
#pragma once
#include "component.h"
#include "entity.h"
#include "reflect.h"
#include <string>

namespace ae {

class ModelComponent;

class TwoBoneIKComponent : public Component {
public:
    std::string endBone;   // effector joint name (glTF node name)
    EntityRef targetRef;
    std::string targetName; // legacy/authoring fallback (see camera rigs)
    EntityRef poleRef;
    std::string poleName;
    float weight = 1.0f;    // 0 = animation only, 1 = full IK

    const char* typeName() const override { return "TwoBoneIK"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("bone", endBone, {PropKind::Default, "End bone"});
        v.visit("targetGuid", "targetName", targetRef, targetName,
                {PropKind::Default, "Target"});
        v.visit("poleGuid", "poleName", poleRef, poleName,
                {PropKind::Default, "Pole (optional)"});
        v.visit("weight", weight, {PropKind::SliderNorm, "Weight"});
    }

    void onLateUpdate(float dt) override;

private:
    ModelComponent* findModel(Entity** modelEntity);
    bool warned_ = false;
};

} // namespace ae
