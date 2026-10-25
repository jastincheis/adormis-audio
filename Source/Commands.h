#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

// Every menu command, its default shortcuts and its place in the menu bar.
// Shortcuts follow the Sound Forge Pro defaults; on macOS "Ctrl" becomes Cmd
// (juce::ModifierKeys::commandModifier).
namespace wf
{
enum CommandIDs : juce::CommandID
{
    // File
    fileNew = 0x1000, fileNewNoDialog, fileOpen, fileClose, fileSave, fileSaveAs, fileSaveAll,
    fileProperties, fileExit,

    // Edit
    editUndo = 0x1100, editRedo, editRepeat, editCut, editCopy, editPaste, editPasteToNew,
    editMix, editDelete, editTrim, editSelectAll, editSetSelection, editMarkIn, editMarkOut,
    editGoTo, editSnapEnable, editSnapGrid, editSnapMarkers, editSnapZero, editNormalTool,
    editPrevTool, editNextTool, editHalveSelection, editDoubleSelection, editOverwrite,

    // View
    viewZoomIn = 0x1200, viewZoomOut, viewZoomSelection, viewZoomNormal, viewLevelIn,
    viewLevelOut, viewPlaybackScroll, viewSmoothScroll, viewDockBottom, viewDockSides,
    viewDockAll, viewMaxWidth, viewExplorer, viewProperties, viewChannelMeters,
    viewUndoHistory, viewSpectrum, viewRegionsList, viewPlaylist, viewLoudnessMeters,

    // Insert
    insertMarker = 0x1300, insertRegion, insertCommandMarker, insertSilence, insertVolumeEnv,
    insertPanEnv, insertLoop,

    // Process
    procAutoTrim = 0x1400, procBitDepth, procChannelConv, procDcOffset, procEqGraphic,
    procEqParametric, procFadeIn, procFadeOut, procFadeGraphic, procInvert, procMute,
    procNormalize, procPan, procResample, procReverse, procSmooth, procTimeStretch, procVolume,

    // Effects
    fxChorus = 0x1500, fxDelay, fxDistortion, fxCompressor, fxLimiter, fxGate, fxFlange,
    fxGapper, fxPitchBend, fxPitchShift, fxReverb, fxVibrato,

    // Tools
    toolsAutoRegion = 0x1600, toolsExtractRegions, toolsClipDetect, toolsFind,
    toolsRepairInterpolate, toolsRepairReplace, toolsRepairCopyChannel, toolsSynthSimple,
    toolsSynthFm, toolsBassFinder, toolsDeEsser, toolsMp3Restore, toolsStatistics,
    toolsNoiseReduction, toolsCaptureNoise, toolsHumRemoval, toolsDeclick, toolsClipRestore, toolsBatch, toolsDrumRebuilder, toolsGetStems,

    // FX Favorites
    favPluginManager = 0x1700, favPluginChain, favRescan,

    // Options (incl. transport)
    optPlay = 0x1800, optPlayAll, optPlayPause, optStop, optRecord, optArm, optLoop,
    optSwitchPlayMode, optGoStart, optGoEnd, optPreferences, optAudioDevice, optKeyboard,
    optGoLeftEdge, optGoRightEdge, optRecordComputer, optCursorLeft, optCursorRight, optExtendLeft, optExtendRight, optPlayFromCursor,
    optMoveSelectionLeft, optMoveSelectionRight,

    // Window
    winCascade = 0x1900, winTile, winNext, winPrev, winMaximizeAll,

    // Help
    helpShortcuts = 0x1a00, helpAbout, helpCheckUpdates, helpUpdatesAtStart
};

struct CommandDef
{
    juce::CommandID id;
    const char* name;
    const char* category;
    std::vector<juce::KeyPress> keys;
    bool implemented = false;
};

const std::vector<CommandDef>& allCommands();
const CommandDef* findCommand (juce::CommandID id);

juce::StringArray menuBarNames();
juce::PopupMenu buildMenu (int topLevelIndex, juce::ApplicationCommandManager& manager);

// Submenus whose items change while the program runs (File > Open Recent):
// asked for by title when the menu opens.
void setDynamicSubmenus (std::function<juce::PopupMenu (const juce::String& title)> provider);

// Human-readable shortcut text using Sound Forge's naming ("Ctrl+O"),
// or the platform's own symbols on macOS.
juce::String shortcutText (const CommandDef& def);
juce::String shortcutText (const juce::Array<juce::KeyPress>& keys);
} // namespace wf
