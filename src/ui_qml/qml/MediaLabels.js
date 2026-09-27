.pragma library

// Shared file-name and parent-folder labels. Main.qml receives file URLs from the shell
// while the image workspace receives controller paths, so the URL form is canonicalized
// with localPathFromUrl before the raw-path helpers run. Both call sites used to carry
// their own copy of this parsing and had already drifted apart.

function fileName(pathValue) {
    const text = String(pathValue || "");
    const slash = Math.max(text.lastIndexOf("/"), text.lastIndexOf("\\"));
    return slash >= 0 ? text.substring(slash + 1) : text;
}

// Parent-folder part of a path, used to disambiguate same-named files from different
// folders. Empty when the path carries no separator.
function parentFolderLabel(pathValue) {
    const text = String(pathValue || "");
    const slash = Math.max(text.lastIndexOf("/"), text.lastIndexOf("\\"));
    if (slash < 0)
        return "";
    const parent = text.substring(0, slash);
    const parentSlash = Math.max(parent.lastIndexOf("/"), parent.lastIndexOf("\\"));
    return parentSlash >= 0 ? parent.substring(parentSlash + 1) : parent;
}

// Decodes a file URL and strips its scheme prefix, yielding the local path form.
function localPathFromUrl(fileUrl) {
    let path = decodeURIComponent(String(fileUrl));
    if (path.startsWith("file:///"))
        path = path.substring(8);
    else if (path.startsWith("file://"))
        path = path.substring(7);
    return path;
}
