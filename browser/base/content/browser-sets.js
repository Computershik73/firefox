/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

document.addEventListener(
  "MozBeforeInitialXULLayout",
  () => {
    // Keep the canonical browser chrome URL: actors such as Urlbar match it exactly.
    if (Services.env.get("GECKO_W10M_XBOX_GAMEPAD") === "1") {
      const actions = document.getElementById("page-action-buttons");
      if (actions) {
        const button = document.createXULElement("toolbarbutton");
        button.id = "strawfox-gamepad-button";
        button.setAttribute("tooltiptext", "Game Controls");
        button.setAttribute("aria-label", "Game Controls");
        const icon =
          '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="context-fill" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round">' +
          '<path d="M5 4.5h6c1.5 0 2.3 1 2.7 2.6l.9 3.4c.4 1.7-1.3 2.5-2.4 1.5l-1.7-1.6h-5L3.8 12c-1.1 1-2.8.2-2.4-1.5l.9-3.4C2.7 5.5 3.5 4.5 5 4.5Z"/>' +
          '<path d="M5 6.5v2.6M3.7 7.8h2.6m4.2-.9h.1m1.4 1.7h.1"/></svg>';
        button.style.listStyleImage =
          "url(\"data:image/svg+xml," + encodeURIComponent(icon) + "\")";
        button.style.setProperty("-moz-context-properties", "fill");
        button.style.fill = "currentColor";
        button.addEventListener("command", () => {
          Services.obs.notifyObservers(null, "gecko-w10m-bridge", "gamepad-controls");
        });
        actions.insertBefore(button, document.getElementById("pageActionButton"));
      }
    }
    const shellOptions = window.arguments?.[1];
    const shellWindowId = shellOptions instanceof Ci.nsIPropertyBag2 &&
      shellOptions.hasKey("desktopmode-window")
      ? shellOptions.getPropertyAsAString("desktopmode-window") : null;
    const shellAppMode = shellWindowId !== null && shellOptions.hasKey("desktopmode-app") &&
      shellOptions.getPropertyAsBool("desktopmode-app");
    const shellManaged =
      Services.prefs.getBoolPref("widget.gecko-w10m.desktop-mode", false) &&
      /^[1-9][0-9]*$/.test(shellWindowId || "");
    if (shellManaged && shellAppMode) {
      document.documentElement.setAttribute("desktopmode-app", "true");
      document.documentElement.style.minWidth = "0px";
      document.documentElement.style.minHeight = "0px";
      document.getElementById("navigator-toolbox")?.setAttribute("hidden", "true");
      document.getElementById("titlebar")?.setAttribute("hidden", "true");
    }
    const sendManagedCommand = command => {
      const id = Cc["@mozilla.org/supports-string;1"].createInstance(Ci.nsISupportsString);
      id.data = shellWindowId;
      Services.obs.notifyObservers(id, "gecko-w10m-managed-window-command", String(command));
    };
    if (shellManaged && shellAppMode) {
      let shellContext = null;
      document.addEventListener("popupshowing", event => {
        if (event.target.id !== "contentAreaContextMenu") return;
        const context = gContextMenu;
        event.preventDefault();
        event.stopPropagation();
        if (!context || !context.shouldDisplay) return;
        shellContext = context;
        const flags = (context.isContentSelected || context.onTextInput ? 1 : 0) |
          (context.onTextInput ? 2 : 0) | (context.onImage ? 4 : 0);
        sendManagedCommand(100 + flags);
      });
      const actionObserver = (subject, topic, action) => {
        if (subject?.QueryInterface(Ci.nsISupportsString).data !== shellWindowId ||
            !shellContext) return;
        gContextMenu = shellContext;
        const items = { copy: "context-copy", paste: "context-paste", saveimage: "context-saveimage" };
        const item = document.getElementById(items[action]);
        if (item && !item.hidden && !item.disabled) item.doCommand();
        shellContext = null;
        gContextMenu = null;
      };
      Services.obs.addObserver(actionObserver, "gecko-w10m-managed-context-action");
      window.addEventListener("unload", () => {
        Services.obs.removeObserver(actionObserver, "gecko-w10m-managed-context-action");
        shellContext = null;
      }, { once: true });
    }
    if (shellManaged) {
      const navigation = (subject, topic, data) => {
        if (subject?.QueryInterface(Ci.nsISupportsString).data !== shellWindowId) return;
        const uri = Services.io.newURI(data);
        if (!uri.schemeIs("https") && !uri.schemeIs("http")) return;
        if (shellAppMode) {
          gBrowser.loadURI(uri, { triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal() });
        } else {
          gBrowser.selectedTab = gBrowser.addTrustedTab(data, { skipAnimation: true });
        }
      };
      const ready = subject => {
        if (subject !== window) return;
        const id = Cc["@mozilla.org/supports-string;1"].createInstance(Ci.nsISupportsString);
        id.data = shellWindowId;
        Services.obs.notifyObservers(id, "gecko-w10m-managed-navigation-ready", "ready");
      };
      Services.obs.addObserver(navigation, "gecko-w10m-managed-open-url");
      Services.obs.addObserver(ready, "browser-delayed-startup-finished");
      window.addEventListener("unload", () => {
        Services.obs.removeObserver(navigation, "gecko-w10m-managed-open-url");
        Services.obs.removeObserver(ready, "browser-delayed-startup-finished");
      }, { once: true });
    }
    const lazy = {};
    ChromeUtils.defineESModuleGetters(lazy, {
      TranslationsParent: "resource://gre/actors/TranslationsParent.sys.mjs",
      AIWindowUI:
        "moz-src:///browser/components/aiwindow/ui/modules/AIWindowUI.sys.mjs",
      ContainerCreationPanel:
        "chrome://browser/content/usercontext/ContainerCreationPanel.mjs",
      Referrals: "resource:///modules/referrals/Referrals.sys.mjs",
    });

    if (
      Services.prefs.getBoolPref("widget.gecko-w10m.desktop-mode", false)
    ) {
      const sessionCloser = {
        observe() {
          if (shellManaged) return;
          // XRE is process-global inside DesktopMode and cannot safely be
          // initialized twice. Replace the live browser session with one clean
          // home tab so closing releases pages, media and their content state
          // while keeping the embedded runtime reusable.
          const cleanTab = gBrowser.addTrustedTab("about:home", {
            skipAnimation: true,
          });
          gBrowser.selectedTab = cleanTab;
          for (const tab of [...gBrowser.tabs]) {
            if (tab !== cleanTab) {
              gBrowser.removeTab(tab, {
                animate: false,
                skipPermitUnload: true,
              });
            }
          }
        },
      };
      Services.obs.addObserver(
        sessionCloser,
        "gecko-w10m-close-browser-session"
      );
      window.addEventListener(
        "unload",
        () =>
          Services.obs.removeObserver(
            sessionCloser,
            "gecko-w10m-close-browser-session"
          ),
        { once: true }
      );
    }

    // <commandset id="mainCommandSet"> defined in browser-sets.inc.xhtml
    document
      .getElementById("mainCommandSet")
      // eslint-disable-next-line complexity
      .addEventListener("command", event => {
        switch (event.target.id) {
          case "cmd_newNavigator":
            if (AIWindow.isDefaultWindow) {
              AIWindow.launchWindow(
                gBrowser?.selectedBrowser,
                true,
                "keyboard_shortcut"
              );
            } else {
              OpenBrowserWindow();
            }
            break;
          case "cmd_handleBackspace":
            BrowserCommands.handleBackspace();
            break;
          case "cmd_handleShiftBackspace":
            BrowserCommands.handleShiftBackspace();
            break;
          case "cmd_newNavigatorTab":
            BrowserCommands.openTab({ event });
            break;
          case "cmd_newNavigatorTabNoEvent":
            BrowserCommands.openTab();
            break;
          case "Browser:OpenFile":
            BrowserCommands.openFileWindow();
            break;
          case "Browser:SavePage":
            saveBrowser(gBrowser.selectedBrowser);
            break;
          case "Browser:SendLink":
            MailIntegration.sendLinkForBrowser(gBrowser.selectedBrowser);
            break;
          case "cmd_pageSetup":
            PrintUtils.showPageSetup();
            break;
          case "cmd_print":
            PrintUtils.startPrintWindow(
              gBrowser.selectedBrowser.browsingContext
            );
            break;
          case "cmd_printPreviewToggle":
            PrintUtils.togglePrintPreview(
              gBrowser.selectedBrowser.browsingContext
            );
            break;
          case "cmd_file_importFromAnotherBrowser":
            MigrationUtils.showMigrationWizard(window, {
              entrypoint: MigrationUtils.MIGRATION_ENTRYPOINTS.FILE_MENU,
            });
            break;
          case "cmd_help_importFromAnotherBrowser":
            MigrationUtils.showMigrationWizard(window, {
              entrypoint: MigrationUtils.MIGRATION_ENTRYPOINTS.HELP_MENU,
            });
            break;
          case "cmd_close":
            BrowserCommands.closeTabOrWindow(event);
            break;
          case "cmd_closeWindow":
            if (shellManaged) { sendManagedCommand(4); break; }
            if (
              Services.prefs.getBoolPref(
                "widget.gecko-w10m.desktop-mode",
                false
              )
            ) {
              Services.obs.notifyObservers(
                null,
                "gecko-w10m-bridge",
                '{"op":"window.close"}'
              );
              break;
            }
            BrowserCommands.tryToCloseWindow(event);
            break;
          case "cmd_returnToOpener":
            BrowserCommands.returnToOpenerFromPiP(event);
            break;
          case "cmd_minimizeWindow":
            if (shellManaged) { sendManagedCommand(1); break; }
            if (
              Services.prefs.getBoolPref(
                "widget.gecko-w10m.desktop-mode",
                false
              )
            ) {
              Services.obs.notifyObservers(
                null,
                "gecko-w10m-bridge",
                '{"op":"window.minimize"}'
              );
              break;
            }
            window.minimize();
            break;
          case "cmd_maximizeWindow":
            if (shellManaged) { sendManagedCommand(2); break; }
            if (
              Services.prefs.getBoolPref(
                "widget.gecko-w10m.desktop-mode",
                false
              )
            ) {
              // Keep the real Gecko widget state in sync with DesktopMode.
              // Merely changing the XUL attribute makes the caption button
              // continue behaving as if the window were restored.
              window.maximize();
              Services.obs.notifyObservers(
                null,
                "gecko-w10m-bridge",
                '{"op":"window.maximize"}'
              );
              break;
            }
            window.maximize();
            break;
          case "cmd_restoreWindow":
            if (shellManaged) { sendManagedCommand(3); break; }
            if (
              Services.prefs.getBoolPref(
                "widget.gecko-w10m.desktop-mode",
                false
              )
            ) {
              window.restore();
              Services.obs.notifyObservers(
                null,
                "gecko-w10m-bridge",
                '{"op":"window.restore"}'
              );
              break;
            }
            window.fullScreen ? BrowserCommands.fullScreen() : window.restore();
            break;
          case "cmd_toggleMute":
            gBrowser.toggleMuteAudioOnMultiSelectedTabs(gBrowser.selectedTab);
            break;
          case "cmd_CustomizeToolbars":
            gCustomizeMode.enter();
            break;
          case "cmd_toggleOfflineStatus":
            BrowserOffline.toggleOfflineStatus();
            break;
          case "cmd_quitApplication":
            goQuitApplication(event);
            break;
          case "View:AboutProcesses":
            switchToTabHavingURI("about:processes", true);
            break;
          case "View:PageSource":
            BrowserCommands.viewSource(window.gBrowser.selectedBrowser);
            break;
          case "View:PageInfo":
            BrowserCommands.pageInfo();
            break;
          case "View:FullScreen":
            BrowserCommands.fullScreen();
            break;
          case "View:ReaderView":
            AboutReaderParent.toggleReaderMode(event);
            break;
          case "View:PictureInPicture":
            PictureInPicture.onCommand(event);
            break;
          case "cmd_find":
            gLazyFindCommand("onFindCommand");
            break;
          case "cmd_findAgain":
            gLazyFindCommand("onFindAgainCommand", false);
            break;
          case "cmd_findPrevious":
            gLazyFindCommand("onFindAgainCommand", true);
            break;
          case "cmd_findSelection":
            gLazyFindCommand("onFindSelectionCommand");
            break;
          case "cmd_translate":
            FullPageTranslationsPanel.open(event);
            break;
          case "cmd_openAboutTranslations":
            lazy.TranslationsParent.openAboutTranslationsPage({
              browserWindow: window,
              targetLanguage: "derive",
            }).catch(console.error);
            break;
          case "cmd_editPDF":
            switchToTabHavingURI("about:pdf", true);
            break;
          case "cmd_openReferrals":
            lazy.Referrals.openReferralsTab(window, "app_menu");
            break;
          case "Browser:AddBookmarkAs":
            PlacesCommandHook.bookmarkPage();
            break;
          case "Browser:SearchBookmarks":
            PlacesCommandHook.searchBookmarks();
            break;
          case "Browser:BookmarkAllTabs":
            PlacesCommandHook.bookmarkTabs();
            break;
          case "Browser:Back":
            BrowserCommands.back();
            break;
          case "Browser:BackOrBackDuplicate":
            BrowserCommands.back(event);
            break;
          case "Browser:Forward":
            BrowserCommands.forward();
            break;
          case "Browser:ForwardOrForwardDuplicate":
            BrowserCommands.forward(event);
            break;
          case "Browser:Stop":
            BrowserCommands.stop();
            break;
          case "Browser:Reload":
            if (event.shiftKey) {
              BrowserCommands.reloadSkipCache();
            } else {
              BrowserCommands.reload();
            }
            break;
          case "Browser:ReloadOrDuplicate":
            BrowserCommands.reloadOrDuplicate(event);
            break;
          case "Browser:ReloadSkipCache":
            BrowserCommands.reloadSkipCache();
            break;
          case "Browser:DuplicateTab":
            BrowserCommands.duplicateTab();
            break;
          case "Browser:NextTab":
            gBrowser.tabContainer.advanceSelectedTab(
              1,
              true,
              event.sourceEvent
            );
            break;
          case "Browser:PrevTab":
            gBrowser.tabContainer.advanceSelectedTab(
              -1,
              true,
              event.sourceEvent
            );
            break;
          case "Browser:ShowAllTabs":
            gTabsPanel.showAllTabsPanel();
            break;
          case "Browser:AddTabSplitView":
            BrowserCommands.addTabSplitView();
            break;
          case "Browser:SeparateTabSplitView":
            BrowserCommands.separateTabSplitView();
            break;
          case "cmd_fullZoomReduce":
            FullZoom.reduce();
            break;
          case "cmd_fullZoomEnlarge":
            FullZoom.enlarge();
            break;
          case "cmd_fullZoomReset":
            FullZoom.reset();
            FullZoom.resetScalingZoom();
            break;
          case "cmd_fullZoomToggle":
            ZoomManager.toggleZoom();
            break;
          case "cmd_gestureRotateLeft":
            gGestureSupport.rotate(event.sourceEvent);
            break;
          case "cmd_gestureRotateRight":
            gGestureSupport.rotate(event.sourceEvent);
            break;
          case "cmd_gestureRotateEnd":
            gGestureSupport.rotateEnd();
            break;
          case "Browser:OpenLocation":
            openLocation(event);
            break;
          case "Browser:RestoreLastSession":
            SessionStore.restoreLastSession();
            break;
          case "Browser:NewUserContextTab":
            openNewUserContextTab(event.sourceEvent);
            break;
          case "Browser:AddContainer":
            lazy.ContainerCreationPanel.open(window);
            break;
          case "Browser:OpenAboutContainers":
            openPreferences("paneContainers");
            break;
          // deliberate fallthrough
          case "Profiles:CreateProfile":
          case "Profiles:ManageProfiles":
          case "Profiles:LaunchProfile":
          case "Profiles:MoveTabsToProfile":
            gProfiles.handleCommand(event);
            break;
          case "Tools:Search":
            SearchUIUtils.webSearch(window);
            break;
          case "Tools:Downloads":
            BrowserCommands.downloadsUI();
            break;
          case "Tools:Addons":
            BrowserAddonUI.openAddonsMgr();
            if (event.sourceEvent?.target.id == "key_openAddons") {
              Services.prefs.setStringPref(
                "browser.keys.openAddons.lastUsed",
                new Date().toISOString()
              );
            }
            break;
          case "cmd_openUnifiedExtensionsPanel":
            gUnifiedExtensions.openPanel(event);
            break;
          case "Tools:ClassicWindow":
            OpenBrowserWindow({ aiWindow: false });
            break;
          case "Tools:AIWindow":
            AIWindow.launchWindow(gBrowser?.selectedBrowser, true, "menu");
            break;
          case "Tools:ChatsHistory":
            FirefoxViewHandler.openTab("chats");
            break;
          case "Tools:Sanitize":
            Sanitizer.showUI(window);
            break;
          case "Tools:PrivateBrowsing":
            OpenBrowserWindow({ private: true });
            break;
          case "Browser:Screenshot":
            ScreenshotsUtils.notify(window, "Shortcut");
            break;
          case "History:UndoCloseTab":
            SessionWindowUI.undoCloseTab(window);
            break;
          case "History:UndoCloseWindow":
            SessionWindowUI.undoCloseWindow();
            break;
          case "History:RestoreLastClosedTabOrWindowOrSession":
            SessionWindowUI.restoreLastClosedTabOrWindowOrSession(window);
            break;
          case "History:SearchHistory":
            PlacesCommandHook.searchHistory();
            break;
          case "wrCaptureCmd":
            gGfxUtils.webrenderCapture();
            break;
          case "wrToggleCaptureSequenceCmd":
            gGfxUtils.toggleWebrenderCaptureSequence();
            break;
          case "windowRecordingCmd":
            gGfxUtils.toggleWindowRecording();
            break;
          case "zoomWindow":
            zoomWindow();
            break;
        }
      });

    document.getElementById("mainKeyset").addEventListener("command", event => {
      const SIDEBAR_REVAMP_PREF = "sidebar.revamp";
      const SIDEBAR_REVAMP_ENABLED = Services.prefs.getBoolPref(
        SIDEBAR_REVAMP_PREF,
        false
      );
      switch (event.target.id) {
        case "goHome":
          BrowserCommands.home();
          break;
        case "bookmarkAllTabsKb":
          PlacesCommandHook.bookmarkTabs();
          break;
        case "viewBookmarksSidebarKb":
          SidebarController.toggle("viewBookmarksSidebar");
          break;
        case "viewOpenTabsSidebarKb":
          SidebarController.toggle("viewOpenTabsSidebar");
          break;
        case "viewBookmarksToolbarKb":
          BookmarkingUI.toggleBookmarksToolbar("shortcut");
          break;
        case "viewGenaiChatSidebarKb": {
          const currentURI = window.gBrowser.selectedBrowser.currentURI;
          const isSmartWindowFullPageMode =
            AIWindow.isAIWindowContentPage(currentURI);
          if (AIWindow.isAIWindowActive(window) && !isSmartWindowFullPageMode) {
            lazy.AIWindowUI.toggleSidebar(window);
            break;
          }

          const pref = "browser.ml.chat.enabled";
          const enabled = Services.prefs.getBoolPref(pref);
          Glean.genaiChatbot.keyboardShortcut.record({
            enabled,
            sidebar: SidebarController.currentID,
          });
          if (enabled) {
            SidebarController.toggle("viewGenaiChatSidebar");
          }
          break;
        }
        case "toggleSidebarKb":
          if (SIDEBAR_REVAMP_ENABLED) {
            SidebarController.handleToolbarButtonClick();
            Glean.sidebar.keyboardShortcut.record({
              panel: SidebarController.currentID,
              opened: SidebarController._state.launcherExpanded,
            });
          }
          break;
        case "key_gotoHistory":
          SidebarController.toggle("viewHistorySidebar");
          break;

        case "key_selectTab1":
        case "key_selectTab2":
        case "key_selectTab3":
        case "key_selectTab4":
        case "key_selectTab5":
        case "key_selectTab6":
        case "key_selectTab7":
        case "key_selectTab8": {
          let index = event.target.id.at(-1) - 1;
          gBrowser.selectTabAtIndex(index, {
            event,
            metricsContext: gBrowser.TabMetrics.userTriggeredContext(
              gBrowser.TabMetrics.METRIC_SOURCE.KEYBOARD
            ),
          });
          break;
        }
        case "key_selectLastTab": {
          gBrowser.selectTabAtIndex(-1, {
            event,
            metricsContext: gBrowser.TabMetrics.userTriggeredContext(
              gBrowser.TabMetrics.METRIC_SOURCE.KEYBOARD
            ),
          });
          break;
        }

        case "key_openHelpMac":
          openHelpLink("firefox-osxkey");
          break;
      }
    });
  },
  { once: true }
);
