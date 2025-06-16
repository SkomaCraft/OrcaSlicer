#include "CraftbotLink.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include <openssl/sha.h>
#include <wx/string.h>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "slic3r/GUI/I18N.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"


namespace Slic3r {

CraftbotLink::CraftbotLink(DynamicPrintConfig* config)
{
    // You can read these from config if needed
    if (config) {
        m_host     = config->opt_string("print_host");  //"10.0.1.91"; 
        m_username = config->opt_string("printhost_user");  //"craft";
        m_password = config->opt_string("printhost_password");//"craftunique";
    }
}

const char* CraftbotLink::get_name() const { return "Craftbot"; }

bool CraftbotLink::test(wxString& curl_msg) const
{
    bool success = true;
    auto url     = make_url("remoteupload");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Testing connection to %2%") % get_name() % url;

    auto http = Http::get(url);
    set_auth(http);

    http.on_error([&](std::string body, std::string error, unsigned status) {
        curl_msg = format_error(body, error, status);
        success = false;
    });

    http.on_complete([&](std::string body, unsigned status) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: GET %2% succeeded. Status: %3%") % get_name() % url % status;
    });

    http.perform_sync();
    return success;
}

wxString CraftbotLink::get_test_failed_msg(wxString& msg) const { return wxString::Format(_L("Could not reach Craftbot device: %s"), msg); }

wxString CraftbotLink::get_test_ok_msg() const { return _L("Craftbot device is reachable."); }

std::string CraftbotLink::get_host() const { return m_host; }

bool CraftbotLink::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    return send_file(upload_data, progress_fn, error_fn, info_fn);
}
bool CraftbotLink::send_file(const PrintHostUpload& upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    // ---- AUTH: Craftbot-style (double SHA + base64) ----
    std::string pwd_sha   = calc_sha256("flow_admin_" + m_password);
    std::string final_sha = calc_sha256("-" + m_username + "-" + pwd_sha + "-");
    std::string auth      = base64_encode(m_username + ":" + final_sha);

    // Load file contents
    std::ifstream file(upload_data.source_path.string(), std::ios::binary);
    if (!file) {
        error_fn("Failed to open file: " + upload_data.source_path.string());
        return false;
    }
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    BOOST_LOG_TRIVIAL(info) << "Craftbot: Read " << data.size() << " bytes for upload.";

    // Override default headers BEFORE creating request
    Http::set_extra_headers({
        {"User-Agent", "CraftWare"} // must be before request
    });

    const std::string url  = "http://" + m_host + "/remoteupload";
    auto              http = Http::post(url);

    http.remove_header("Accept");


    http.header("Content-Type", "application/octet-stream");
    http.header("Content-Length", std::to_string(data.size())); // ADD THIS IN ORDER
    http.header("Name", upload_data.upload_path.filename().string());
    http.header("Authorization", "Basic " + auth);
    http.header("User-Agent", "CraftWare"); // just to be sure
    http.header("Host", m_host);            // manually set it too
    http.header("Cache-Control", "no-cache");

    // Set POST body
    http.set_post_body(std::string(data.data(), data.size()));

    bool success = true;

    http.on_progress([&](Http::Progress progress, bool& cancel) {
        progress_fn(std::move(progress), cancel);
        if (cancel) {
            success = false;
            BOOST_LOG_TRIVIAL(info) << "Craftbot: Upload canceled by user.";
        }
    });

    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << "Craftbot: Upload failed. HTTP " << status << ", error: " << error << ", body: " << body;
        error_fn(format_error(body, error, status));
        success = false;
    });

    http.on_complete([&](std::string body, unsigned status) {
        BOOST_LOG_TRIVIAL(info) << "Craftbot: Upload complete. HTTP " << status;
        info_fn("craftbot", _L("Upload successful"));
    });

    http.perform_sync();

    return success;
}


std::string CraftbotLink::calc_sha256(const std::string& input) const
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), hash);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    return oss.str();
}

std::string CraftbotLink::base64_encode(const std::string& input) const
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string           output;
    int                   val = 0, valb = -6;
    for (uint8_t c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            output.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        output.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (output.size() % 4)
        output.push_back('=');
    return output;
}

void CraftbotLink::set_auth(Http& http) const
{
    std::string pwd_sha   = calc_sha256("flow_admin_" + m_password);
    std::string final_sha = calc_sha256("-" + m_username + "-" + pwd_sha + "-");
    std::string auth      = base64_encode(m_username + ":" + final_sha);
    http.header("Authorization", "Basic " + auth);
}

std::string CraftbotLink::make_url(const std::string& path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0)
        return m_host.back() == '/' ? m_host + path : m_host + "/" + path;
    else
        return "http://" + m_host + "/" + path;
}

}
