#ifndef STATICHTMLPAGE_H__
#define STATICHTMLPAGE_H__
#include "../../../ontology.h"
#include <string>

class StaticHtmlPage :
    public HtmlPageBase<StaticHtmlPage>, public EphemeralBase<StaticHtmlPage>,
    public DeletableBase<StaticHtmlPage>
{
public:
    WIRE_TYPE_IDENTITY(StaticHtmlPage);

    // The three fixed sub-paths this page answers to. Previously lived on
    // FileHtmlPage (kMountCssArg/kMountJsArg) because only its mount branch
    // consulted them -- but they were always describing what a STATIC PAGE
    // serves, not what a tree node does with one. Moved here, where the
    // knowledge belongs, which is what lets FileHtmlPage's mount branch
    // collapse to a single forwarding call.
    static constexpr const char* kIndexPath = "/";
    static constexpr const char* kHtmlPath  = "/index.html";
    static constexpr const char* kCssPath   = "/style.css";
    static constexpr const char* kJsPath    = "/app.js";

    StaticHtmlPage() = default;
    virtual ~StaticHtmlPage() = default;

    bool ResetConcrete()
    {
        html_content_.reset();
        css_content_.reset();
        js_content_.reset();
        return true;
    }

    bool IsActiveConcrete()     const { return true; }
    bool IsFileBackedConcrete() const { return false; }

    // The three assets, addressed by fixed path. Non-owning: the returned
    // pointers are into this page's own NBuffers and stay valid only while it
    // is alive and unmodified -- which is exactly the "always re-read current
    // content" property a live-editable page wants, since a Set*Raw between
    // requests is picked up with no re-registration anywhere.
    //
    // A leading-slash-less path is accepted too, so this works identically
    // whether it is reached directly (a server routing "/style.css") or via a
    // tree mount (FileHtmlPage handing down a bare segment).
    HtmlPage_::ResolvedAsset ResolveConcrete(const std::string& request_path) const
    {
        HtmlPage_::ResolvedAsset result;

        std::string p = request_path;
        if (!p.empty() && p.front() != '/') p.insert(p.begin(), '/');

        const ETCS::NBuffer* payload = nullptr;
        const char*          mime    = nullptr;

        if (p.empty() || p == kIndexPath || p == kHtmlPath)
        { payload = &html_content_; mime = "text/html"; }
        else if (p == kCssPath)
        { payload = &css_content_;  mime = "text/css"; }
        else if (p == kJsPath)
        { payload = &js_content_;   mime = "application/javascript"; }

        // written == 0 is a genuine miss, not an empty 200: an unset asset is
        // one this page does not serve, and reporting it as matched would turn
        // every unconfigured page into a silent blank response.
        if (!payload || payload->written == 0) return result;

        result.matched   = true;
        result.data      = payload->buf;
        result.length    = payload->written;
        result.mime_type = mime;
        return result;
    }

    bool DeleteConcrete()
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("StaticHtmlPage", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }
};

#endif // STATICHTMLPAGE_H__
