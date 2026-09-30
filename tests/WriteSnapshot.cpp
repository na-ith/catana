#include "catan/Debug.h"
#include "catan/Game.h"
#include "catan/Mcts.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: catan_snapshot OUTPUT.json\n";
        return 2;
    }
    catan::Game game(42);
    catan::MctsConfig config;
    config.iterations = 128;
    config.maxRolloutSteps = 16;
    config.seed = 42;
    const auto analysis = catan::MctsEngine(config).search(game.state());
    std::string error;
    if (!catan::debug::writeStateJson(game.state(), analysis, std::filesystem::path(argv[1]), &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}
