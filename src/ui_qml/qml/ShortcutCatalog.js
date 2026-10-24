.pragma library

// Single source of truth for workspace-level shortcut sequences. Main.qml binds them, the
// menu bar displays them and the shortcut help lists them, all from these constants, and a
// contract test cross-checks bindings, menu labels and help rows against each other - the
// three surfaces cannot drift apart. Media transport keys live in ReviewShortcuts.qml, next
// to their own bindings.
const openVideos = "Ctrl+O";
const openVideoFolder = "Ctrl+Alt+O";
const addVideo = "Ctrl+Shift+O";
const openImagePair = "Ctrl+Shift+I";
const addImage = "Ctrl+Alt+I";
const compareImageFolders = "Ctrl+Shift+F";
const closeCurrentTask = "Ctrl+W";
const nextImageTab = "Ctrl+Tab";
const previousImageTab = "Ctrl+Shift+Tab";
// Same sequences as the image-tab pair by design (video task tabs design §8): the two
// workspaces bind them as mutually exclusive Shortcuts - imageWorkspaceActive versus the
// video workspace - so one key pair cycles whichever task strip is on screen.
const nextVideoTab = "Ctrl+Tab";
const previousVideoTab = "Ctrl+Shift+Tab";
const captureIssue = "M";
// Displayed by the menu only: the window manager owns Alt+F4 delivery, no app Shortcut
// binds it. The contract test knows this one exception.
const quit = "Alt+F4";
