#include "dialogue_player.h"
#include <algorithm>

namespace ae {

void DialoguePlayer::start() {
    visited_.clear();
    if (scene_.startNode.empty() || scene_.nodes.empty()) { sub_ = SubState::Done; return; }
    enterNode(scene_.startNode);
}

void DialoguePlayer::enterNode(const std::string& id) {
    current_ = id.empty() ? nullptr : scene_.find(id);
    if (!current_) { sub_ = SubState::Done; return; }
    visited_.push_back(current_->id);

    switch (current_->type) {
    case NodeType::Line:
        sub_ = SubState::Line;
        lineDur_ = std::max(0.1f, current_->duration);
        lineT_ = lineDur_;
        break;
    case NodeType::Choice: {
        std::vector<ChoiceOption> opts;
        opts.reserve(current_->options.size());
        for (const auto& o : current_->options) opts.push_back({o.text});
        choice_.begin(std::move(opts), current_->timeLimit);
        sub_ = SubState::Choice;
        break;
    }
    case NodeType::Qte: {
        std::vector<QteStep> steps;
        for (int k : current_->qteKeys) steps.push_back({k});
        if (steps.empty()) steps.push_back({'F'});
        qte_.begin(current_->qteType, std::move(steps), current_->qteDuration,
                  current_->qteHoldSeconds, current_->qteMashCount);
        pendingQte_ = QteResult::None;
        sub_ = SubState::Qte;
        break;
    }
    case NodeType::End:
        sub_ = SubState::Done;
        break;
    }
}

void DialoguePlayer::drawLine(UI& ui, const Rect& area, float alpha01) {
    float a = clampf(alpha01, 0.0f, 1.0f);
    std::string full = current_->speaker.empty() ? current_->text
                                                  : current_->speaker + ":  " + current_->text;
    float w = ui.measureText(full.c_str(), 1.0f) + 48;
    Rect r{area.x + area.w * 0.5f - w * 0.5f, area.y + area.h * 0.78f, w, 40};
    ui.rectFill(r, rgba(0, 0, 0, 0.62f * a));
    ui.rectFill({r.x, r.y, 3, r.h}, rgba(0.55f, 0.45f, 0.96f, a));
    ui.textCentered(r, full.c_str(), rgba(1, 1, 1, a));
}

void DialoguePlayer::update(UI& ui, float dt, const Rect& area) {
    switch (sub_) {
    case SubState::Done:
        break;

    case SubState::Line: {
        float fadeIn = clampf((lineDur_ - lineT_) / 0.2f, 0.0f, 1.0f);
        float fadeOut = clampf(lineT_ / 0.2f, 0.0f, 1.0f);
        drawLine(ui, area, std::min(fadeIn, fadeOut));
        lineT_ -= dt;
        if (lineT_ <= 0.0f) enterNode(current_->next);
        break;
    }

    case SubState::Choice: {
        ChoiceResult r = choice_.tick(ui, dt, area);
        if (r == ChoiceResult::Picked)
            enterNode(current_->options[choice_.selectedIndex()].target);
        else if (r == ChoiceResult::TimedOut)
            enterNode(current_->timeoutTarget);
        break;
    }

    case SubState::Qte: {
        QteResult r = qte_.tick(ui, dt, area);
        if (r != QteResult::None) pendingQte_ = r;
        if (!qte_.active())
            enterNode(pendingQte_ == QteResult::Success ? current_->successTarget : current_->failTarget);
        break;
    }
    }
}

} // namespace ae
