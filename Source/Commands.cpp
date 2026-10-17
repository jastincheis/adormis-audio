#include "Commands.h"

namespace wf
{
namespace
{
using juce::KeyPress;

constexpr int ctrl  = juce::ModifierKeys::commandModifier; // Cmd on macOS
constexpr int shift = juce::ModifierKeys::shiftModifier;
constexpr int alt   = juce::ModifierKeys::altModifier;     // Option on macOS

KeyPress key (int code, int mods = 0)  { return KeyPress (code, juce::ModifierKeys (mods), 0); }

std::vector<KeyPress> exitKeys()
{
   #if JUCE_MAC
    return { key ('q', ctrl) };
   #else
    return { key (KeyPress::F4Key, alt) };
   #endif
}

std::vector<CommandDef> makeCommands()
{
    return {
        // File
        { fileNew,          "New...",                         "File", { key ('n', ctrl) } },
        { fileNewNoDialog,  "New (skip dialog)",              "File", { key ('n', ctrl | shift) } },
        { fileOpen,         "Open...",                        "File", { key ('o', ctrl) } },
        { fileClose,        "Close",                          "File", { key ('w', ctrl) } },
        { fileSave,         "Save",                           "File", { key ('s', ctrl) } },
        { fileSaveAs,       "Save As...",                     "File", {} },
        { fileSaveAll,      "Save All",                       "File", {} },
        { fileProperties,   "Properties",                     "File", { key (KeyPress::returnKey, alt) } },
        { fileExit,         "Exit",                           "File", exitKeys(), true },

        // Edit
        { editUndo,         "Undo",                           "Edit", { key ('z', ctrl) } },
        { editRedo,         "Redo",                           "Edit", { key ('z', ctrl | shift) } },
        { editRepeat,       "Repeat Last Process",            "Edit", { key ('y', ctrl) } },
        { editCut,          "Cut",                            "Edit", { key ('x', ctrl) } },
        { editCopy,         "Copy",                           "Edit", { key ('c', ctrl) } },
        { editPaste,        "Paste",                          "Edit", { key ('v', ctrl) } },
        { editPasteToNew,   "Paste to New",                   "Edit", { key ('e', ctrl) } },
        { editMix,          "Mix...",                         "Edit", { key ('m', ctrl) } },
        // Backspace too: the "delete" key on Mac keyboards sends Backspace.
        { editDelete,       "Delete (Clear)",                 "Edit", { key (KeyPress::deleteKey), key (KeyPress::backspaceKey) } },
        { editTrim,         "Trim/Crop",                      "Edit", { key ('t', ctrl) } },
        { editSelectAll,    "Select All",                     "Edit", { key ('a', ctrl) } },
        { editSetSelection, "Set Selection...",               "Edit", { key ('d', ctrl | shift) } },
        { editMarkIn,       "Set Mark In",                    "Edit", { key ('i') } },
        { editMarkOut,      "Set Mark Out",                   "Edit", { key ('o') } },
        { editGoTo,         "Go To...",                       "Edit", { key ('g', ctrl) } },
        { editSnapEnable,   "Enable Snapping",                "Edit", { key (KeyPress::F8Key) } },
        { editSnapGrid,     "Auto Snap to Grid",              "Edit", { key (KeyPress::F8Key, ctrl) } },
        { editSnapMarkers,  "Auto Snap to Markers",           "Edit", { key (KeyPress::F8Key, shift) } },
        { editSnapZero,     "Auto Snap to Zero Crossings",    "Edit", { key ('b', ctrl) } },
        { editNormalTool,   "Normal Edit Tool",               "Edit", { key ('d', ctrl) } },
        { editPrevTool,     "Previous Edit Tool",             "Edit", { key ('d') } },
        { editNextTool,     "Next Edit Tool",                 "Edit", { key ('d', shift) } },

        // View
        { viewZoomIn,         "Zoom In (Time)",               "View", { key (KeyPress::upKey) } },
        { viewZoomOut,        "Zoom Out (Time)",              "View", { key (KeyPress::downKey) } },
        { viewZoomSelection,  "Zoom to Selection",            "View", { key (KeyPress::upKey, ctrl) } },
        { viewZoomNormal,     "Zoom Normal",                  "View", { key (KeyPress::downKey, ctrl) } },
        { viewLevelIn,        "Level Zoom In",                "View", { key (KeyPress::upKey, shift) } },
        { viewLevelOut,       "Level Zoom Out",               "View", { key (KeyPress::downKey, shift) } },
        { viewPlaybackScroll, "Playback Scrolling",           "View", { key (KeyPress::F6Key) } },
        { viewSmoothScroll,   "Smooth Playback Scrolling",    "View", { key (KeyPress::F6Key, shift) } },
        { viewDockBottom,     "Show/Hide Bottom Docked Windows", "View", { key (KeyPress::F11Key) } },
        { viewDockSides,      "Show/Hide Side Docked Windows",   "View", { key (KeyPress::F11Key, shift) } },
        { viewDockAll,        "Show/Hide All Docked Windows",    "View", { key (KeyPress::F11Key, ctrl) } },
        { viewMaxWidth,       "Maximize Width",               "View", { key (KeyPress::returnKey, ctrl) } },
        { viewExplorer,       "Explorer",                     "View", { key ('1', alt) } },
        { viewProperties,     "File Properties",              "View", { key ('2', alt) } },
        { viewChannelMeters,  "Channel Meters",               "View", { key ('5', alt) } },
        { viewLoudnessMeters, "Loudness Meters",              "View", { key ('6', alt) } },
        { viewUndoHistory,    "Undo/Redo History",            "View", { key ('8', alt) } },
        { viewSpectrum,       "Spectrum Analysis",            "View", { key ('9', alt) } },
        { viewRegionsList,    "Regions List",                 "View", {} },
        { viewPlaylist,       "Playlist/Cutlist",             "View", {} },

        // Insert
        { insertMarker,        "Marker",                      "Insert", { key ('m') } },
        { insertRegion,        "Region",                      "Insert", { key ('r') } },
        { insertCommandMarker, "Command Marker",              "Insert", { key ('c') } },
        { insertSilence,       "Silence...",                  "Insert", {} },
        { insertVolumeEnv,     "Volume Envelope",             "Insert", { key ('v') } },
        { insertPanEnv,        "Pan Envelope",                "Insert", { key ('p') } },
        { insertLoop,          "Sample Loop",                 "Insert", { key ('l', alt) } },

        // Process
        { procAutoTrim,     "Auto Trim/Crop...",              "Process", {} },
        { procBitDepth,     "Bit-Depth Converter...",         "Process", {} },
        { procChannelConv,  "Channel Converter...",           "Process", {} },
        { procDcOffset,     "DC Offset...",                   "Process", {} },
        { procEqGraphic,    "Graphic EQ...",                  "Process", {} },
        { procEqParametric, "Parametric EQ...",               "Process", {} },
        { procFadeIn,       "Fade In",                        "Process", {} },
        { procFadeOut,      "Fade Out",                       "Process", {} },
        { procFadeGraphic,  "Graphic Fade...",                "Process", {} },
        { procInvert,       "Invert/Flip",                    "Process", {} },
        { procMute,         "Mute",                           "Process", {} },
        { procNormalize,    "Normalize...",                   "Process", {} },
        { procPan,          "Pan/Expand...",                  "Process", {} },
        { procResample,     "Resample...",                    "Process", {} },
        { procReverse,      "Reverse",                        "Process", {} },
        { procSmooth,       "Smooth/Enhance...",              "Process", {} },
        { procTimeStretch,  "Time Stretch...",                "Process", {} },
        { procVolume,       "Volume...",                      "Process", {} },

        // Effects
        { fxChorus,      "Chorus...",                         "Effects", {} },
        { fxDelay,       "Delay/Echo...",                     "Effects", {} },
        { fxDistortion,  "Distortion...",                     "Effects", {} },
        { fxCompressor,  "Compressor...",                     "Effects", {} },
        { fxLimiter,     "Limiter...",                        "Effects", {} },
        { fxGate,        "Noise Gate...",                     "Effects", {} },
        { fxFlange,      "Flanger...",                        "Effects", {} },
        { fxGapper,      "Gapper/Snipper...",                 "Effects", {} },
        { fxPitchBend,   "Pitch Bend...",                     "Effects", {} },
        { fxPitchShift,  "Pitch Shift...",                    "Effects", {} },
        { fxReverb,      "Reverb...",                         "Effects", {} },
        { fxVibrato,     "Vibrato...",                        "Effects", {} },

        // Tools
        { toolsAutoRegion,        "Auto Region...",           "Tools", {} },
        { toolsExtractRegions,    "Extract Regions...",       "Tools", {} },
        { toolsClipDetect,        "Detect Clipping...",       "Tools", {} },
        { toolsFind,              "Find...",                  "Tools", {} },
        { toolsRepairInterpolate, "Interpolate",              "Tools", {} },
        { toolsRepairReplace,     "Replace",                  "Tools", {} },
        { toolsRepairCopyChannel, "Copy Other Channel",       "Tools", {} },
        { toolsSynthSimple,       "Simple Synthesis...",      "Tools", {} },
        { toolsSynthFm,           "FM Synthesis...",          "Tools", {} },
        { toolsBassFinder,        "Bass Finder...",           "Tools", {} },
        { toolsDeEsser,           "De-esser...",              "Tools", {} },
        { toolsMp3Restore,        "MP3 Restore...",           "Tools", {} },
        { toolsStatistics,        "Statistics...",            "Tools", {} },
        { toolsNoiseReduction,    "Noise Reduction...",       "Tools", {} },
        { toolsCaptureNoise,      "Capture Noise Print",      "Tools", {} },
        { toolsHumRemoval,        "Hum Removal...",           "Tools", {} },
        { toolsDeclick,           "Click and Crackle Removal...", "Tools", {} },
        { toolsClipRestore,       "Clipped Peak Restoration...",  "Tools", {} },
        { toolsBatch,             "Batch Converter...",       "Tools", {} },

        // FX Favorites
        { favPluginManager, "Plug-In Manager...",             "FX Favorites", {} },
        { favPluginChain,   "Plug-In Chain...",               "FX Favorites", {} },
        { favRescan,        "Rescan VST3 Plug-Ins",           "FX Favorites", {} },

        // Options / transport
        { optPlay,           "Play/Stop",                     "Transport", { key (KeyPress::spaceKey), key (KeyPress::F12Key) } },
        { optPlayFromCursor, "Play from Cursor",              "Transport", {} },
        { optPlayAll,        "Play All",                      "Transport", { key (KeyPress::spaceKey, shift), key (KeyPress::F12Key, shift) } },
        { optPlayPause,      "Play/Pause",                    "Transport", { key (KeyPress::returnKey), key (KeyPress::F12Key, ctrl) } },
        { optStop,           "Stop",                          "Transport", { key (KeyPress::escapeKey) } },
        { optRecord,         "Record",                        "Transport", { key ('r', ctrl) } },
        { optRecordComputer, "Record What the Computer Plays", "Transport", { key ('r', ctrl | shift) } },
        { optArm,            "Arm for Recording",             "Transport", { key ('a', ctrl | shift) } },
        { optLoop,           "Loop Playback",                 "Transport", { key ('q') } },
        { optSwitchPlayMode, "Switch Play Mode",              "Transport", { key ('x') } },
        { optGoStart,        "Go to Start",                   "Transport", { key (KeyPress::homeKey, ctrl) } },
        { optGoEnd,          "Go to End",                     "Transport", { key (KeyPress::endKey, ctrl) } },
        { optGoLeftEdge,     "Go to Left Edge",               "Transport", { key ('w') } },
        { optGoRightEdge,    "Go to Right Edge",              "Transport", { key ('e') } },
        { optCursorLeft,     "Move Cursor Left",              "Transport", { key (KeyPress::leftKey) } },
        { optCursorRight,    "Move Cursor Right",             "Transport", { key (KeyPress::rightKey) } },
        { optExtendLeft,     "Extend Selection Left",         "Transport", { key (KeyPress::leftKey, shift) } },
        { optExtendRight,    "Extend Selection Right",        "Transport", { key (KeyPress::rightKey, shift) } },
        { optPreferences,    "Preferences...",                "Options", {} },
        { optAudioDevice,    "Audio Device...",               "Options", {} },
        { optKeyboard,       "Customize Keyboard...",         "Options", {} },

        // Window
        { winCascade,     "Cascade",                          "Window", { key (KeyPress::F5Key, shift) } },
        { winTile,        "Tile Vertically",                  "Window", { key (KeyPress::F4Key, shift) } },
        { winNext,        "Next Window",                      "Window", { key (KeyPress::F6Key, ctrl) } },
        { winPrev,        "Previous Window",                  "Window", { key (KeyPress::F6Key, ctrl | shift) } },
        { winMaximizeAll, "Maximize All",                     "Window", { key (KeyPress::F10Key, ctrl) } },

        // Help
        { helpShortcuts, "Keyboard Shortcuts",                "Help", { key (KeyPress::F1Key) }, true },
        { helpAbout,     "About Adormis Audio",                   "Help", {}, true },
    };
}

struct MenuItem
{
    enum class Kind { command, separator, submenu };
    Kind kind;
    juce::CommandID id = 0;
    const char* title = nullptr;
    std::vector<MenuItem> children;
};

MenuItem C (juce::CommandID id)                                    { return { MenuItem::Kind::command, id, nullptr, {} }; }
MenuItem sep()                                                     { return { MenuItem::Kind::separator, 0, nullptr, {} }; }
MenuItem sub (const char* title, std::vector<MenuItem> children)   { return { MenuItem::Kind::submenu, 0, title, std::move (children) }; }

struct TopMenu
{
    const char* name;
    std::vector<MenuItem> items;
};

const std::vector<TopMenu>& menuBar()
{
    static const std::vector<TopMenu> menus {
        { "File", { C (fileNew), C (fileOpen), sub ("Open Recent", {}), C (fileClose), sep(),
                    C (fileSave), C (fileSaveAs), C (fileSaveAll), sep(),
                    C (fileProperties), sep(), C (fileExit) } },

        { "Edit", { C (editUndo), C (editRedo), C (editRepeat), sep(),
                    C (editCut), C (editCopy), C (editPaste),
                    sub ("Paste Special", { C (editPasteToNew), C (editMix) }),
                    C (editDelete), C (editTrim), sep(),
                    C (editSelectAll), C (editSetSelection), C (editMarkIn), C (editMarkOut), sep(),
                    C (editGoTo), sep(),
                    sub ("Snapping", { C (editSnapEnable), sep(), C (editSnapGrid), C (editSnapMarkers), C (editSnapZero) }),
                    sub ("Tool", { C (editNormalTool), C (editPrevTool), C (editNextTool) }) } },

        { "View", { C (viewZoomIn), C (viewZoomOut), C (viewZoomSelection), C (viewZoomNormal), sep(),
                    C (viewLevelIn), C (viewLevelOut), sep(),
                    C (viewPlaybackScroll), C (viewSmoothScroll), C (viewMaxWidth), sep(),
                    C (viewExplorer), C (viewProperties), C (viewChannelMeters), C (viewLoudnessMeters), C (viewUndoHistory),
                    C (viewSpectrum), C (viewRegionsList), C (viewPlaylist), sep(),
                    sub ("Docked Windows", { C (viewDockBottom), C (viewDockSides), C (viewDockAll) }) } },

        { "Insert", { C (insertMarker), C (insertRegion), C (insertCommandMarker), sep(),
                      C (insertSilence), sep(),
                      C (insertVolumeEnv), C (insertPanEnv), sep(), C (insertLoop) } },

        { "Process", { C (procAutoTrim), C (procBitDepth), C (procChannelConv), C (procDcOffset),
                       sub ("EQ", { C (procEqGraphic), C (procEqParametric) }),
                       sub ("Fade", { C (procFadeIn), C (procFadeOut), C (procFadeGraphic) }),
                       C (procInvert), C (procMute), C (procNormalize), C (procPan), C (procResample),
                       C (procReverse), C (procSmooth), C (procTimeStretch),
                       C (procVolume) } },

        { "Effects", { C (fxChorus), C (fxDelay), C (fxDistortion),
                       sub ("Dynamics", { C (fxCompressor), C (fxLimiter), C (fxGate) }),
                       C (fxFlange), C (fxGapper),
                       sub ("Pitch", { C (fxPitchBend), C (fxPitchShift) }),
                       C (fxReverb), C (fxVibrato) } },

        { "Tools", { C (toolsAutoRegion), C (toolsExtractRegions), C (toolsClipDetect), C (toolsFind), C (toolsStatistics), sep(),
                     sub ("Noise Reduction", { C (toolsNoiseReduction), C (toolsCaptureNoise), sep(),
                                               C (toolsHumRemoval), C (toolsDeclick), C (toolsClipRestore) }),
                     sub ("Repair", { C (toolsRepairInterpolate), C (toolsRepairReplace), C (toolsRepairCopyChannel) }),
                     sub ("Synthesis", { C (toolsSynthSimple), C (toolsSynthFm) }), sep(), C (toolsBassFinder), C (toolsDeEsser), C (toolsMp3Restore), sep(), C (toolsBatch) } },

        { "FX Favorites", { C (favPluginManager), C (favPluginChain), sep(), C (favRescan) } },

        { "Options", { sub ("Transport", { C (optPlay), C (optPlayFromCursor), C (optPlayAll), C (optPlayPause), C (optStop), sep(),
                                           C (optRecord), C (optRecordComputer), C (optArm), sep(),
                                           C (optLoop), C (optSwitchPlayMode), sep(),
                                           C (optGoStart), C (optGoEnd), C (optGoLeftEdge), C (optGoRightEdge), sep(),
                                           C (optCursorLeft), C (optCursorRight), C (optExtendLeft), C (optExtendRight) }),
                       sep(), C (optAudioDevice), C (optKeyboard), C (optPreferences) } },

        { "Window", { C (winCascade), C (winTile), C (winMaximizeAll), sep(), C (winNext), C (winPrev) } },

        { "Help", { C (helpShortcuts), sep(), C (helpAbout) } },
    };
    return menus;
}

std::function<juce::PopupMenu (const juce::String&)>& dynamicSubmenus()
{
    static std::function<juce::PopupMenu (const juce::String&)> provider;
    return provider;
}

juce::PopupMenu build (const std::vector<MenuItem>& items, juce::ApplicationCommandManager& manager)
{
    juce::PopupMenu menu;

    for (const auto& item : items)
    {
        switch (item.kind)
        {
            case MenuItem::Kind::command:   menu.addCommandItem (&manager, item.id); break;
            case MenuItem::Kind::separator: menu.addSeparator(); break;
            case MenuItem::Kind::submenu:
                if (item.children.empty() && dynamicSubmenus() != nullptr)
                {
                    const auto sub = dynamicSubmenus() (item.title);
                    menu.addSubMenu (item.title, sub, sub.getNumItems() > 0);
                }
                else
                {
                    menu.addSubMenu (item.title, build (item.children, manager));
                }
                break;
        }
    }

    return menu;
}
} // namespace

void setDynamicSubmenus (std::function<juce::PopupMenu (const juce::String&)> provider)
{
    dynamicSubmenus() = std::move (provider);
}

const std::vector<CommandDef>& allCommands()
{
    // Commands that work so far; everything else stays greyed out in the menus.
    static const juce::CommandID implemented[] {
        fileNew, fileOpen, fileClose, fileSave, fileSaveAs, fileExit, editUndo, editRedo, editCut, editCopy, editPaste, editPasteToNew, editDelete, editTrim, editSelectAll,
        fileNewNoDialog, fileSaveAll, editRepeat, editMix, editSetSelection, editMarkIn, editMarkOut, editGoTo, editSnapZero, viewPlaybackScroll, viewSmoothScroll,
        viewLevelIn, viewLevelOut, insertSilence, procAutoTrim, procBitDepth, procChannelConv, procResample,
        toolsClipDetect, toolsRepairCopyChannel, optLoop, insertMarker,
        viewChannelMeters, viewLoudnessMeters, viewSpectrum, toolsStatistics, optKeyboard,
        fxLimiter, fxGate, fxChorus, fxFlange, fxVibrato, fxDistortion, fxPitchShift,
        toolsNoiseReduction, toolsCaptureNoise, toolsHumRemoval, toolsDeclick, toolsClipRestore,
        insertRegion, viewRegionsList, toolsAutoRegion, toolsExtractRegions, toolsBatch,
        procFadeIn, procFadeOut, procMute, procNormalize, procReverse, procInvert, procDcOffset, procVolume, procTimeStretch, toolsBassFinder, toolsDeEsser, toolsMp3Restore, optRecord, optRecordComputer,
        procEqParametric, procEqGraphic, procFadeGraphic, insertVolumeEnv, fxCompressor, fxDelay, fxReverb,
        winCascade, winTile, winNext, winPrev,
        viewZoomIn, viewZoomOut, viewZoomSelection, viewZoomNormal,
        optPlay, optPlayAll, optPlayPause, optStop, optGoStart, optGoEnd, optGoLeftEdge, optGoRightEdge, optAudioDevice,
        optCursorLeft, optCursorRight, optExtendLeft, optExtendRight, optPlayFromCursor,
        helpShortcuts, helpAbout
    };

    static const auto commands = []
    {
        auto list = makeCommands();

        for (auto& def : list)
            def.implemented = std::find (std::begin (implemented), std::end (implemented), def.id) != std::end (implemented);

        return list;
    }();

    return commands;
}

const CommandDef* findCommand (juce::CommandID id)
{
    for (const auto& def : allCommands())
        if (def.id == id)
            return &def;

    return nullptr;
}

juce::StringArray menuBarNames()
{
    juce::StringArray names;

    for (const auto& menu : menuBar())
        names.add (menu.name);

    return names;
}

juce::PopupMenu buildMenu (int topLevelIndex, juce::ApplicationCommandManager& manager)
{
    const auto& menus = menuBar();

    if (! juce::isPositiveAndBelow (topLevelIndex, (int) menus.size()))
        return {};

    return build (menus[(size_t) topLevelIndex].items, manager);
}

juce::String shortcutText (const CommandDef& def)
{
    juce::Array<juce::KeyPress> keys;

    for (const auto& k : def.keys)
        keys.add (k);

    return shortcutText (keys);
}

juce::String shortcutText (const juce::Array<juce::KeyPress>& keys)
{
    juce::StringArray parts;

    for (const auto& k : keys)
    {
       #if JUCE_MAC
        parts.add (k.getTextDescriptionWithIcons());
       #else
        // JUCE writes "ctrl + cursor up"; Sound Forge writes "Ctrl+Up Arrow".
        juce::StringArray tokens;
        tokens.addTokens (k.getTextDescription(), "+", {});
        tokens.trim();

        for (auto& t : tokens)
        {
            if      (t == "ctrl")        t = "Ctrl";
            else if (t == "shift")       t = "Shift";
            else if (t == "alt")         t = "Alt";
            else if (t == "spacebar")    t = "Spacebar";
            else if (t == "return")      t = "Enter";
            else if (t == "escape")      t = "Esc";
            else if (t == "delete")      t = "Delete";
            else if (t == "backspace")   t = "Backspace";
            else if (t == "home")        t = "Home";
            else if (t == "end")         t = "End";
            else if (t.startsWith ("cursor "))
                t = t.substring (7).substring (0, 1).toUpperCase() + t.substring (8) + " Arrow";
        }

        parts.add (tokens.joinIntoString ("+"));
       #endif
    }

    return parts.joinIntoString (", ");
}
} // namespace wf
