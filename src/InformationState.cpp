#include "catan/InformationState.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace catan {
namespace {
auto worldKey(const GameState& state) {
    return std::tuple{state.players[0].developmentCards.cards,
        state.players[0].developmentCards.boughtThisTurn,
        state.players[1].developmentCards.cards,
        state.players[1].developmentCards.boughtThisTurn,
        state.developmentDeck.cards, state.turn.phase, state.winner};
}
unsigned cardCount(const auto& cards) {
    return std::accumulate(cards.begin(), cards.end(), 0U);
}
}

InformationState::InformationState(GameState knownPrior, PlayerId observer)
    : observer_(observer), worlds_{{std::move(knownPrior), 1.0}} {
    if (index(observer) >= PlayerCount) throw std::invalid_argument("invalid information-state observer");
    refreshPublicState();
}

void InformationState::refreshPublicState() {
    // A hidden VP draw can end a possible world. Until victory is observed,
    // expose ongoing play if any compatible world is still ongoing.
    const auto ongoing = std::find_if(worlds_.begin(), worlds_.end(),
        [](const auto& world) { return !world.state.winner; });
    public_ = (ongoing == worlds_.end() ? worlds_.front() : *ongoing).state;
    public_.players[index(opponent(observer_))].developmentCards = {};
    public_.developmentDeck.cards.fill(0);
}

std::vector<Action> InformationState::discardChoices(PlayerId player) const {
    return Game(worlds_.front().state).discardChoices(player);
}

std::vector<Action> InformationState::legalTransformations() const {
    if (public_.winner) return {};
    if (public_.turn.phase == Phase::DevelopmentDraw && decisionPlayer() != observer_)
        return {DrawUnknownDevelopmentCard{}};
    std::vector<Action> result;
    for (const auto& world : worlds_) {
        if (world.state.winner) continue;
        for (const auto& action : Game(world.state).legalTransformations())
            if (std::find(result.begin(), result.end(), action) == result.end()) result.push_back(action);
    }
    return result;
}

std::vector<WeightedAction> InformationState::chanceOutcomes() const {
    if (public_.winner) return {};
    if (public_.turn.phase == Phase::DevelopmentDraw && decisionPlayer() != observer_)
        return {{DrawUnknownDevelopmentCard{}, 1.0}};
    std::vector<WeightedAction> result;
    double mass = 0.0;
    for (const auto& world : worlds_) {
        if (world.state.winner) continue;
        mass += world.probability;
        for (const auto& outcome : Game(world.state).chanceOutcomes()) {
            const auto found = std::find_if(result.begin(), result.end(),
                [&](const auto& item) { return item.action == outcome.action; });
            if (found == result.end()) result.push_back({outcome.action, world.probability * outcome.probability});
            else found->probability += world.probability * outcome.probability;
        }
    }
    if (mass > 0.0) for (auto& outcome : result) outcome.probability /= mass;
    return result;
}

StepResult InformationState::transform(const Action& observation) {
    const bool hiddenDraw = std::holds_alternative<DrawUnknownDevelopmentCard>(observation);
    if (hiddenDraw && (public_.turn.phase != Phase::DevelopmentDraw || decisionPlayer() == observer_))
        return {false, "Only an opponent development draw can be unknown", {}};
    if (std::holds_alternative<DrawDevelopmentCard>(observation) && decisionPlayer() != observer_)
        return {false, "Opponent draw identities are hidden; use draw unknown development card", {}};
    using Key = decltype(worldKey(public_));
    std::map<Key, InformationWorld> merged;
    std::vector<std::string> events;
    for (const auto& world : worlds_) {
        if (const auto* victory = std::get_if<ObserveVictory>(&observation)) {
            if (world.state.winner == victory->player) merged.emplace(worldKey(world.state), world);
            continue;
        }
        Game game(world.state);
        std::vector<WeightedAction> possibilities;
        if (hiddenDraw) possibilities = game.chanceOutcomes();
        else {
            double likelihood = 1.0;
            if (isChanceAction(observation)) {
                likelihood = 0.0;
                for (const auto& outcome : game.chanceOutcomes())
                    if (outcome.action == observation) likelihood = outcome.probability;
            }
            if (likelihood > 0.0) possibilities.push_back({observation, likelihood});
        }
        for (const auto& possibility : possibilities) {
            Game branch = game;
            const auto resolution = branch.transform(possibility.action);
            if (!resolution.accepted) continue;
            if (events.empty()) events = resolution.events;
            const auto key = worldKey(branch.state());
            const auto [it, inserted] = merged.try_emplace(key,
                InformationWorld{branch.state(), 0.0});
            if (!inserted && it->second.state != branch.state())
                throw std::logic_error("different public resolutions in an information transformation");
            it->second.probability += world.probability * possibility.probability;
        }
    }
    double mass = 0.0;
    for (const auto& [key, world] : merged) { (void)key; mass += world.probability; }
    if (!(mass > 0.0) || !std::isfinite(mass))
        return {false, "Observation is incompatible with every possible world", {}};
    worlds_.clear();
    for (auto& [key, world] : merged) {
        (void)key;
        world.probability /= mass;
        worlds_.push_back(std::move(world));
    }
    refreshPublicState();
    if (hiddenDraw) events = {"opponent development card remains unknown; belief updated"};
    if (std::holds_alternative<ObserveVictory>(observation)) events = {"observed game victory"};
    // Do not report a win from one hidden-card hypothesis as an observed win.
    if (!public_.winner) std::erase_if(events,
        [](const std::string& event) { return event.find(" wins with ") != std::string::npos; });
    return {true, {}, std::move(events)};
}

GameState InformationState::determinize(double quantile) const {
    quantile = std::clamp(quantile, 0.0, std::nextafter(1.0, 0.0));
    double sum = 0.0;
    for (const auto& world : worlds_) {
        sum += world.probability;
        if (quantile < sum) return world.state;
    }
    return worlds_.back().state;
}
bool InformationState::contains(const GameState& candidate) const {
    return std::any_of(worlds_.begin(), worlds_.end(),
        [&](const auto& world) { return world.state == candidate; });
}
double InformationState::expectedOpponentCards(DevelopmentCard card) const {
    double result = 0.0;
    for (const auto& world : worlds_)
        result += world.probability * world.state.players[index(opponent(observer_))].developmentCards.cards[index(card)];
    return result;
}
double InformationState::expectedDeckCards(DevelopmentCard card) const {
    double result = 0.0;
    for (const auto& world : worlds_) result += world.probability * world.state.developmentDeck.cards[index(card)];
    return result;
}
unsigned InformationState::opponentCardCount() const {
    return cardCount(worlds_.front().state.players[index(opponent(observer_))].developmentCards.cards);
}
unsigned InformationState::deckCardCount() const {
    return cardCount(worlds_.front().state.developmentDeck.cards);
}
} // namespace catan
