/*  LOOT

    A load order optimisation tool for Oblivion, Skyrim, Fallout 3 and
    Fallout: New Vegas.

    Copyright (C) 2012-2018    WrinklyNinja

    This file is part of LOOT.

    LOOT is free software: you can redistribute
    it and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    LOOT is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with LOOT.  If not, see
    <https://www.gnu.org/licenses/>.
    */

#include "gui/state/game/game.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/locale.hpp>

#include "gui/helpers.h"
#include "gui/state/game/game_detection_error.h"
#include "gui/state/game/helpers.h"
#include "gui/state/logging.h"
#include "loot/exception/file_access_error.h"
#include "loot/exception/undefined_group_error.h"

#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include "shlobj.h"
#include "shlwapi.h"
#include "windows.h"
#endif

using std::list;
using std::lock_guard;
using std::mutex;
using std::string;
using std::thread;
using std::vector;
using std::filesystem::u8path;

namespace fs = std::filesystem;

namespace loot {
namespace gui {
bool hasPluginFileExtension(const std::string& filename) {
  return boost::iends_with(filename, ".esp") ||
         boost::iends_with(filename, ".esm") ||
         boost::iends_with(filename, ".esl");
}

Game::Game(const GameSettings& gameSettings,
           const std::filesystem::path& lootDataPath) :
    GameSettings(gameSettings),
    lootDataPath_(lootDataPath),
    pluginsFullyLoaded_(false),
    loadOrderSortCount_(0),
    logger_(getLogger()) {
  auto gamePath = FindGamePath();
  if (gamePath.has_value()) {
    SetGamePath(gamePath.value());
  } else {
    throw GameDetectionError("Game path could not be detected.");
  }

  gameHandle_ = CreateGameHandle(Type(), GamePath(), GameLocalPath());
  gameHandle_->IdentifyMainMasterFile(Master());
}

Game::Game(const Game& game) :
    GameSettings(game),
    lootDataPath_(game.lootDataPath_),
    gameHandle_(game.gameHandle_),
    pluginsFullyLoaded_(game.pluginsFullyLoaded_),
    messages_(game.messages_),
    loadOrderSortCount_(0),
    logger_(getLogger()) {}

Game& Game::operator=(const Game& game) {
  if (&game != this) {
    GameSettings::operator=(game);

    lootDataPath_ = game.lootDataPath_;
    gameHandle_ = game.gameHandle_;
    pluginsFullyLoaded_ = game.pluginsFullyLoaded_;
    messages_ = game.messages_;
    loadOrderSortCount_ = game.loadOrderSortCount_;
    logger_ = game.logger_;
  }

  return *this;
}

void Game::Init() {
  if (logger_) {
    logger_->info("Initialising filesystem-related data for game: {}", Name());
  }

  if (!lootDataPath_.empty()) {
    // Make sure that the LOOT game path exists.
    auto lootGamePath = lootDataPath_ / u8path(FolderName());
    if (!fs::is_directory(lootGamePath)) {
      if (fs::exists(lootGamePath)) {
        throw FileAccessError(
            "Could not create LOOT folder for game, the path exists but is not "
            "a directory");
      }
      fs::create_directories(lootGamePath);
    }
  }
}

std::shared_ptr<const PluginInterface> Game::GetPlugin(
    const std::string& name) const {
  return gameHandle_->GetPlugin(name);
}

std::set<std::shared_ptr<const PluginInterface>> Game::GetPlugins() const {
  return gameHandle_->GetLoadedPlugins();
}

std::vector<Message> Game::CheckInstallValidity(
    const std::shared_ptr<const PluginInterface>& plugin,
    const PluginMetadata& metadata) {
  if (logger_) {
    logger_->trace(
        "Checking that the current install is valid according to {}'s data.",
        plugin->GetName());
  }
  std::vector<Message> messages;
  if (IsPluginActive(plugin->GetName())) {
    auto fileExists = [&](const std::string& file) {
      return std::filesystem::exists(DataPath() / u8path(file)) ||
             (hasPluginFileExtension(file) &&
              std::filesystem::exists(DataPath() / u8path(file + ".ghost")));
    };
    auto tags = metadata.GetTags();
    if (tags.find(Tag("Filter")) == std::end(tags)) {
      for (const auto& master : plugin->GetMasters()) {
        if (!fileExists(master)) {
          if (logger_) {
            logger_->error("\"{}\" requires \"{}\", but it is missing.",
                           plugin->GetName(),
                           master);
          }
          messages.push_back(Message(MessageType::error,
                                     (boost::format(boost::locale::translate(
                                          "This plugin requires \"%1%\" to be "
                                          "installed, but it is missing.")) %
                                      master)
                                         .str()));
        } else if (!IsPluginActive(master)) {
          if (logger_) {
            logger_->error("\"{}\" requires \"{}\", but it is inactive.",
                           plugin->GetName(),
                           master);
          }
          messages.push_back(Message(MessageType::error,
                                     (boost::format(boost::locale::translate(
                                          "This plugin requires \"%1%\" to be "
                                          "active, but it is inactive.")) %
                                      master)
                                         .str()));
        }
      }
    }

    for (const auto& req : metadata.GetRequirements()) {
      if (!fileExists(req.GetName())) {
        if (logger_) {
          logger_->error("\"{}\" requires \"{}\", but it is missing.",
                         plugin->GetName(),
                         req.GetName());
        }
        messages.push_back(Message(MessageType::error,
                                   (boost::format(boost::locale::translate(
                                        "This plugin requires \"%1%\" to be "
                                        "installed, but it is missing.")) %
                                    req.GetDisplayName())
                                       .str()));
      }
    }
    for (const auto& inc : metadata.GetIncompatibilities()) {
      if (fileExists(inc.GetName()) &&
          (!hasPluginFileExtension(inc.GetName()) ||
           IsPluginActive(inc.GetName()))) {
        if (logger_) {
          logger_->error(
              "\"{}\" is incompatible with \"{}\", but both files are present.",
              plugin->GetName(),
              inc.GetName());
        }
        messages.push_back(
            Message(MessageType::error,
                    (boost::format(boost::locale::translate(
                         "This plugin is incompatible with \"%1%\", but both "
                         "files are present.")) %
                     inc.GetDisplayName())
                        .str()));
      }
    }
  }

  if (plugin->IsLightMaster() &&
      !boost::iends_with(plugin->GetName(), ".esp")) {
    for (const auto& masterName : plugin->GetMasters()) {
      auto master = GetPlugin(masterName);
      if (!master) {
        if (logger_) {
          logger_->info(
              "Tried to get plugin object for master \"{}\" of \"{}\" but it "
              "was not loaded.",
              masterName,
              plugin->GetName());
        }
        continue;
      }

      if (!master->IsLightMaster() && !master->IsMaster()) {
        if (logger_) {
          logger_->error(
              "\"{}\" is a light master and requires the non-master plugin "
              "\"{}\". This can cause issues in-game, and sorting will fail "
              "while this plugin is installed.",
              plugin->GetName(),
              masterName);
        }
        messages.push_back(Message(
            MessageType::error,
            (boost::format(boost::locale::translate(
                 "This plugin is a light master and requires the non-master "
                 "plugin \"%1%\". This can cause issues in-game, and sorting "
                 "will fail while this plugin is installed.")) %
             masterName)
                .str()));
      }
    }
  }

  if (plugin->IsLightMaster() && !plugin->IsValidAsLightMaster()) {
    if (logger_) {
      logger_->error(
          "\"{}\" contains records that have FormIDs outside the valid range "
          "for an ESL plugin. Using this plugin will cause irreversible damage "
          "to your game saves.",
          plugin->GetName());
    }
    messages.push_back(
        Message(MessageType::error,
                boost::locale::translate(
                    "This plugin contains records that have FormIDs outside "
                    "the valid range for an ESL plugin. Using this plugin "
                    "will cause irreversible damage to your game saves.")));
  }

  if (plugin->GetHeaderVersion() < MinimumHeaderVersion()) {
    if (logger_) {
      logger_->warn(
          "\"{}\" has a header version of {}, which is less than the game's "
          "minimum supported header version of {}.",
          plugin->GetName(),
          plugin->GetHeaderVersion(),
          MinimumHeaderVersion());
    }
    messages.push_back(Message(
        MessageType::warn,
        (boost::format(boost::locale::translate(
             "This plugin has a header version of %1%, which is less than the "
             "game's minimum supported header version of %2%.")) %
         plugin->GetHeaderVersion() % MinimumHeaderVersion())
            .str()));
  }

  // Also generate dirty messages.
  for (const auto& element : metadata.GetDirtyInfo()) {
    messages.push_back(ToMessage(element));
  }

  return messages;
}

void Game::RedatePlugins() {
  if (Type() != GameType::tes5 && Type() != GameType::tes5se) {
    if (logger_) {
      logger_->warn("Cannot redate plugins for game {}.", Name());
    }
    return;
  }

  vector<string> loadorder = gameHandle_->GetLoadOrder();
  if (!loadorder.empty()) {
    std::filesystem::file_time_type lastTime =
        std::filesystem::file_time_type::clock::time_point::min();
    for (const auto& pluginName : loadorder) {
      fs::path filepath = DataPath() / u8path(pluginName);
      if (!fs::exists(filepath)) {
        filepath += ".ghost";
        if (!fs::exists(filepath)) {
          continue;
        }
      }

      auto thisTime = fs::last_write_time(filepath);
      if (thisTime >= lastTime) {
        lastTime = thisTime;

        if (logger_) {
          logger_->trace("No need to redate \"{}\".",
                         filepath.filename().u8string());
        }
      } else {
        lastTime += std::chrono::seconds(60);
        fs::last_write_time(filepath,
                            lastTime);  // Space timestamps by a minute.

        if (logger_) {
          logger_->info("Redated \"{}\"", filepath.filename().u8string());
        }
      }
    }
  }
}

void Game::LoadAllInstalledPlugins(bool headersOnly) {
  try {
    gameHandle_->LoadCurrentLoadOrderState();
  } catch (std::exception& e) {
    if (logger_) {
      logger_->error("Failed to load current load order. Details: {}",
                     e.what());
    }
    AppendMessage(Message(
        MessageType::error,
        boost::locale::translate("Failed to load the current load order, "
                                 "information displayed may be incorrect.")
            .str()));
  }

  auto installedPluginNames = GetInstalledPluginNames();
  gameHandle_->LoadPlugins(installedPluginNames, headersOnly);

  // Check if any plugins have been removed.
  std::vector<std::string> loadedPluginNames;
  for (auto plugin : gameHandle_->GetLoadedPlugins()) {
    loadedPluginNames.push_back(plugin->GetName());
  }

  AppendMessages(
      CheckForRemovedPlugins(installedPluginNames, loadedPluginNames));

  pluginsFullyLoaded_ = !headersOnly;
}

bool Game::ArePluginsFullyLoaded() const { return pluginsFullyLoaded_; }

std::filesystem::path Game::DataPath() const {
  if (GamePath().empty())
    throw std::logic_error("Cannot get data path from empty game path");
  return GamePath() / "Data";
}

fs::path Game::MasterlistPath() const {
  return lootDataPath_ / u8path(FolderName()) / "masterlist.yaml";
}

fs::path Game::UserlistPath() const {
  return lootDataPath_ / u8path(FolderName()) / "userlist.yaml";
}

std::vector<std::string> Game::GetLoadOrder() const {
  return gameHandle_->GetLoadOrder();
}

void Game::SetLoadOrder(const std::vector<std::string>& loadOrder) {
  BackupLoadOrder(GetLoadOrder(), lootDataPath_ / u8path(FolderName()));
  gameHandle_->SetLoadOrder(loadOrder);
}

bool Game::IsPluginActive(const std::string& pluginName) const {
  return gameHandle_->IsPluginActive(pluginName);
}

std::optional<short> Game::GetActiveLoadOrderIndex(
    const std::shared_ptr<const PluginInterface>& plugin,
    const std::vector<std::string>& loadOrder) const {
  using boost::locale::to_lower;
  // Get the full load order, then count the number of active plugins until the
  // given plugin is encountered. If the plugin isn't active or in the load
  // order, return nullopt.

  if (!IsPluginActive(plugin->GetName()))
    return std::nullopt;

  short numberOfActivePlugins = 0;
  for (const std::string& otherPluginName : loadOrder) {
    if (to_lower(otherPluginName) == to_lower(plugin->GetName()))
      return numberOfActivePlugins;

    auto otherPlugin = GetPlugin(otherPluginName);
    if (otherPlugin &&
        plugin->IsLightMaster() == otherPlugin->IsLightMaster() &&
        IsPluginActive(otherPluginName)) {
      ++numberOfActivePlugins;
    }
  }

  return std::nullopt;
}

std::vector<std::string> Game::SortPlugins() {
  std::vector<std::string> plugins = GetInstalledPluginNames();

  try {
    gameHandle_->LoadCurrentLoadOrderState();
  } catch (std::exception& e) {
    if (logger_) {
      logger_->error("Failed to load current load order. Details: {}",
                     e.what());
    }
    AppendMessage(Message(
        MessageType::error,
        boost::locale::translate("Failed to load the current load order, "
                                 "information displayed may be incorrect.")
            .str()));
  }

  std::vector<std::string> sortedPlugins;
  try {
    // Clear any existing game-specific messages, as these only relate to
    // state that has been changed by sorting.
    ClearMessages();

    sortedPlugins = gameHandle_->SortPlugins(plugins);

    AppendMessages(CheckForRemovedPlugins(plugins, sortedPlugins));

    IncrementLoadOrderSortCount();
  } catch (CyclicInteractionError& e) {
    if (logger_) {
      logger_->error("Failed to sort plugins. Details: {}", e.what());
    }
    AppendMessage(Message(
        MessageType::error,
        (boost::format(boost::locale::translate(
             "Cyclic interaction detected between \"%1%\" and \"%2%\": %3%")) %
         e.GetCycle().front().GetName() % e.GetCycle().back().GetName() %
         DescribeCycle(e.GetCycle()))
            .str()));
    sortedPlugins.clear();
  } catch (UndefinedGroupError& e) {
    if (logger_) {
      logger_->error("Failed to sort plugins. Details: {}", e.what());
    }
    AppendMessage(Message(MessageType::error,
                          (boost::format(boost::locale::translate(
                               "The group \"%1%\" does not exist.")) %
                           e.GetGroupName())
                              .str()));
    sortedPlugins.clear();
  } catch (std::exception& e) {
    if (logger_) {
      logger_->error("Failed to sort plugins. Details: {}", e.what());
    }
    sortedPlugins.clear();
  }

  return sortedPlugins;
}

void Game::IncrementLoadOrderSortCount() {
  lock_guard<mutex> guard(mutex_);

  ++loadOrderSortCount_;
}

void Game::DecrementLoadOrderSortCount() {
  lock_guard<mutex> guard(mutex_);

  if (loadOrderSortCount_ > 0)
    --loadOrderSortCount_;
}

std::vector<Message> Game::GetMessages() const {
  std::vector<Message> output(
      gameHandle_->GetDatabase()->GetGeneralMessages(true));
  output.insert(end(output), begin(messages_), end(messages_));

  if (loadOrderSortCount_ == 0)
    output.push_back(
        Message(MessageType::warn,
                boost::locale::translate(
                    "You have not sorted your load order this session.")));

  size_t activeNormalPluginsCount = 0;
  bool hasActiveEsl = false;
  for (const auto& plugin : GetPlugins()) {
    if (IsPluginActive(plugin->GetName())) {
      if (plugin->IsLightMaster()) {
        hasActiveEsl = true;
      } else {
        ++activeNormalPluginsCount;
      }
    }
  }

  if (activeNormalPluginsCount > 254 && hasActiveEsl) {
    if (logger_) {
      logger_->warn(
          "255 normal plugins and at least one light master are active at the "
          "same time.");
    }
    output.push_back(Message(
        MessageType::warn,
        boost::locale::translate(
            "You have a normal plugin and at least one light master sharing "
            "the FE load order index. Deactivate a normal plugin or all your "
            "light masters to avoid potential issues.")));
  }

  return output;
}

void Game::AppendMessage(const Message& message) {
  lock_guard<mutex> guard(mutex_);

  messages_.push_back(message);
}

void Game::ClearMessages() {
  lock_guard<mutex> guard(mutex_);

  messages_.clear();
}

bool Game::UpdateMasterlist() {
  bool wasUpdated = gameHandle_->GetDatabase()->UpdateMasterlist(
      MasterlistPath(), RepoURL(), RepoBranch());
  if (wasUpdated && !gameHandle_->GetDatabase()->IsLatestMasterlist(
                        MasterlistPath(), RepoBranch())) {
    AppendMessage(Message(
        MessageType::error,
        boost::locale::translate(
            "The latest masterlist revision contains a syntax error, LOOT is "
            "using the most recent valid revision instead. Syntax errors are "
            "usually minor and fixed within hours.")));
  }

  return wasUpdated;
}

MasterlistInfo Game::GetMasterlistInfo() const {
  return gameHandle_->GetDatabase()->GetMasterlistRevision(MasterlistPath(),
                                                           true);
}

void Game::LoadMetadata() {
  std::filesystem::path masterlistPath;
  std::filesystem::path userlistPath;
  if (std::filesystem::exists(MasterlistPath())) {
    if (logger_) {
      logger_->debug("Preparing to parse masterlist.");
    }
    masterlistPath = MasterlistPath();
  }

  if (std::filesystem::exists(UserlistPath())) {
    if (logger_) {
      logger_->debug("Preparing to parse userlist.");
    }
    userlistPath = UserlistPath();
  }

  if (logger_) {
    logger_->debug("Parsing metadata list(s).");
  }
  try {
    gameHandle_->GetDatabase()->LoadLists(masterlistPath, userlistPath);
  } catch (std::exception& e) {
    if (logger_) {
      logger_->error("An error occurred while parsing the metadata list(s): {}",
                     e.what());
    }
    AppendMessage(Message(
        MessageType::error,
        (boost::format(boost::locale::translate(
             "An error occurred while parsing the metadata list(s): "
             "%1%.\n\nTry updating your masterlist to resolve the error. If "
             "the error is with your user metadata, this probably happened "
             "because an update to LOOT changed its metadata syntax support. "
             "Your user metadata will have to be updated manually.\n\nTo do "
             "so, use the 'Open Debug Log Location' in LOOT's main menu to "
             "open its data folder, then open your 'userlist.yaml' file in the "
             "relevant game folder. You can then edit the metadata it contains "
             "with reference to the documentation, which is accessible through "
             "LOOT's main menu.\n\nYou can also seek support on LOOT's forum "
             "thread, which is linked to on [LOOT's "
             "website](https://loot.github.io/).")) %
         e.what())
            .str()));
  }
}

std::set<std::string> Game::GetKnownBashTags() const {
  return gameHandle_->GetDatabase()->GetKnownBashTags();
}

std::unordered_set<Group> Game::GetMasterlistGroups() const {
  return gameHandle_->GetDatabase()->GetGroups(false);
}

std::unordered_set<Group> Game::GetUserGroups() const {
  return gameHandle_->GetDatabase()->GetUserGroups();
}

std::optional<PluginMetadata> Game::GetMasterlistMetadata(
    const std::string& pluginName,
    bool evaluateConditions) const {
  return gameHandle_->GetDatabase()->GetPluginMetadata(
      pluginName, false, evaluateConditions);
}

std::optional<PluginMetadata> Game::GetUserMetadata(
    const std::string& pluginName,
    bool evaluateConditions) const {
  return gameHandle_->GetDatabase()->GetPluginUserMetadata(pluginName,
                                                           evaluateConditions);
}

void Game::SetUserGroups(const std::unordered_set<Group>& groups) {
  return gameHandle_->GetDatabase()->SetUserGroups(groups);
}

void Game::AddUserMetadata(const PluginMetadata& metadata) {
  gameHandle_->GetDatabase()->SetPluginUserMetadata(metadata);
}

void Game::ClearUserMetadata(const std::string& pluginName) {
  gameHandle_->GetDatabase()->DiscardPluginUserMetadata(pluginName);
}

void Game::ClearAllUserMetadata() {
  gameHandle_->GetDatabase()->DiscardAllUserMetadata();
}

void Game::SaveUserMetadata() {
  gameHandle_->GetDatabase()->WriteUserMetadata(UserlistPath(), true);
}

std::vector<std::string> Game::GetInstalledPluginNames() {
  std::vector<std::string> plugins;

  if (logger_) {
    logger_->trace("Scanning for plugins in {}", this->DataPath().u8string());
  }

  for (fs::directory_iterator it(this->DataPath());
       it != fs::directory_iterator();
       ++it) {
    if (fs::is_regular_file(it->status()) &&
        gameHandle_->IsValidPlugin(it->path().filename().u8string())) {
      string name = it->path().filename().u8string();

      if (logger_) {
        logger_->info("Found plugin: {}", name);
      }

      plugins.push_back(name);
    }
  }

  return plugins;
}

void Game::AppendMessages(std::vector<Message> messages) {
  for (auto message : messages) {
    AppendMessage(message);
  }
}
}
}
