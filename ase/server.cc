// This Source Code Form is licensed MPL-2.0: http://mozilla.org/MPL/2.0
#include "server.hh"
#include "jsonipc/jsonipc.hh"
#include "platform.hh"
#include "properties.hh"
#include "serialize.hh"
#include "main.hh"
#include "utils.hh"
#include "path.hh"
#include "internal.hh"

namespace Ase {

// == Preferences ==
PropertyS
Preferences::access_properties (const EventHandler &eventhandler)
{
  using namespace Properties;
  static PropertyBag bag (eventhandler);
  return_unless (bag.props.empty(), bag.props);
  bag.group = _("Synthesis Settings");
  bag += Text (&pcm_driver, _("PCM Driver"), "", "auto", STANDARD, _("Driver and device to be used for PCM input and output"));
  bag += Range (&synth_latency, _("Latency"), "", 0, 3000, 5, "ms", STANDARD + "step=5",
                _("Processing duration between input and output of a single sample, smaller values increase CPU load"));
  bag += Range (&synth_mixing_freq, _("Synth Mixing Frequency"), "", 48000, 48000, 48000, "Hz", STANDARD,
                _("Unused, synthesis mixing frequency is always 48000 Hz"));
  bag += Range (&synth_control_freq, _("Synth Control Frequency"), "", 1500, 1500, 1500, "Hz", STANDARD,
                _("Unused frequency setting"));
  bag.group = _("MIDI");
  bag += Text (&midi_driver, _("MIDI Driver"), "", STANDARD, _("Driver and device to be used for MIDI input and output"));
  bag += Bool (&invert_sustain, _("Invert Sustain"), "", false, STANDARD,
               _("Invert the state of sustain (damper) pedal so on/off meanings are reversed"));
  bag.group = _("Default Values");
  bag += Text (&author_default, _("Default Author"), "", STANDARD, _("Default value for 'Author' fields"));
  bag += Text (&license_default, _("Default License"), "", STANDARD, _("Default value for 'License' fields"));
  bag.group = _("Search Paths");
  bag += Text (&sample_path, _("Sample Path"), "", STANDARD + "searchpath",
               _("Search path of directories, seperated by \";\", used to find audio samples."));
  bag += Text (&effect_path, _("Effect Path"), "", STANDARD + "searchpath",
               _("Search path of directories, seperated by \";\", used to find effect files."));
  bag += Text (&instrument_path, _("Instrument Path"), "", STANDARD + "searchpath",
               _("Search path of directories, seperated by \";\", used to find instrument files."));
  bag += Text (&plugin_path, _("Plugin Path"), "", STANDARD + "searchpath",
               _("Search path of directories, seperated by \";\", used to find plugins. This path "
                 "is searched for in addition to the standard plugin location on this system."));
  return bag.props;
}

static Preferences
preferences_defaults()
{
  // Server is *not* yet available
  Preferences prefs;
  // static defaults
  prefs.pcm_driver = "auto";
  prefs.synth_latency = 22;
  prefs.synth_mixing_freq = 48000;
  prefs.synth_control_freq = 1500;
  prefs.midi_driver = "auto";
  prefs.invert_sustain = false;
  prefs.license_default = "Creative Commons Attribution-ShareAlike 4.0 (https://creativecommons.org/licenses/by-sa/4.0/)";
  // dynamic defaults
  const String default_user_path = Path::join (Path::user_home(), "Anklang");
  prefs.effect_path     = default_user_path + "/Effects";
  prefs.instrument_path = default_user_path + "/Instruments";
  prefs.plugin_path     = default_user_path + "/Plugins";
  prefs.sample_path     = default_user_path + "/Samples";
  String user = user_name();
  if (!user.empty())
    {
      String name = user_real_name();
      if (!name.empty() && user != name)
        prefs.author_default = name;
      else
        prefs.author_default = user;
    }
  return prefs;
}

static String
pathname_anklangrc()
{
  static const String anklangrc = Path::join (Path::config_home(), "anklang", "anklangrc.json");
  return anklangrc;
}

// == ServerImpl ==
JSONIPC_INHERIT (ServerImpl, Server);

ServerImpl::ServerImpl ()
{
  prefs_ = preferences_defaults();
  const String jsontext = Path::stringread (pathname_anklangrc());
  if (!jsontext.empty())
    json_parse (jsontext, prefs_);
  on_event ("change:prefs", [this] (auto...) {
    Path::stringwrite (pathname_anklangrc(), json_stringify (prefs_, Writ::INDENT | Writ::SKIP_EMPTYSTRING), true);
  });
}

ServerImpl::~ServerImpl ()
{
  fatal_error ("ServerImpl references must persist");
}

String
ServerImpl::get_version ()
{
  return ase_version();
}

String
ServerImpl::get_vorbis_version ()
{
  return "-";
}

String
ServerImpl::get_mp3_version ()
{
  return "-";
}

void
ServerImpl::shutdown ()
{
  // defer quit() slightly, so remote calls are still completed
  main_loop->exec_timer ([] () { main_loop->quit (0); }, 5, -1, EventLoop::PRIORITY_NORMAL);
}

ProjectP
ServerImpl::last_project ()
{
  return Project::last_project();
}

ProjectP
ServerImpl::create_project (String projectname)
{
  return Project::create (projectname);
}

PropertyS
ServerImpl::access_prefs()
{
  return prefs_.access_properties ([this] (const Event&) {
    ValueR args { { "prefs", json_parse<ValueR> (json_stringify (prefs_)) } };
    emit_event ("change", "prefs", args);
  });
}

DriverEntryS
ServerImpl::list_pcm_drivers ()
{
  return {};
}

DriverEntryS
ServerImpl::list_midi_drivers ()
{
  return {};
}

ServerImplP
ServerImpl::instancep ()
{
  static ServerImplP *sptr = new ServerImplP (std::make_shared<ServerImpl>());
  return *sptr;
}

// == Server ==
ServerP
Server::instancep ()
{
  return ServerImpl::instancep();
}

Server&
Server::instance ()
{
  return *instancep();
}

static ValueR session_data;

void
Server::set_session_data (const String &key, const Value &v)
{
  session_data[key] = v;
  // printerr ("%s: %s = %s\n", __func__, key, session_data[key].repr());
}

const Value&
Server::get_session_data (const String &key) const
{
  return session_data[key];
}

// == Error ==
/// Describe Error condition.
const char*
ase_error_blurb (Error error)
{
  switch (error)
    {
    case Error::NONE:			return _("OK");
    case Error::INTERNAL:		return _("Internal error (please report)");
    case Error::UNKNOWN:		return _("Unknown error");
    case Error::IO:			return _("Input/output error");
    case Error::PERMS:			return _("Insufficient permissions");
      // out of resource conditions
    case Error::NO_MEMORY:		return _("Out of memory");
    case Error::MANY_FILES:		return _("Too many open files");
    case Error::NO_FILES:		return _("Too many open files in system");
    case Error::NO_SPACE:		return _("No space left on device");
      // file errors
    case Error::FILE_BUSY:		return _("Device or resource busy");
    case Error::FILE_EXISTS:		return _("File exists already");
    case Error::FILE_EOF:		return _("End of file");
    case Error::FILE_EMPTY:		return _("File empty");
    case Error::FILE_NOT_FOUND:		return _("No such file, device or directory");
    case Error::FILE_IS_DIR:		return _("Is a directory");
    case Error::FILE_OPEN_FAILED:	return _("Open failed");
    case Error::FILE_SEEK_FAILED:	return _("Seek failed");
    case Error::FILE_READ_FAILED:	return _("Read failed");
    case Error::FILE_WRITE_FAILED:	return _("Write failed");
      // content errors
    case Error::NO_HEADER:		return _("Failed to detect header");
    case Error::NO_SEEK_INFO:		return _("Failed to retrieve seek information");
    case Error::NO_DATA_AVAILABLE:	return _("No data available");
    case Error::DATA_CORRUPT:		return _("Data corrupt");
    case Error::WRONG_N_CHANNELS:	return _("Wrong number of channels");
    case Error::FORMAT_INVALID:		return _("Invalid format");
    case Error::FORMAT_UNKNOWN:		return _("Unknown format");
    case Error::DATA_UNMATCHED:		return _("Requested data values unmatched");
      // Device errors
    case Error::DEVICE_NOT_AVAILABLE:   return _("No device (driver) available");
    case Error::DEVICE_ASYNC:		return _("Device not async capable");
    case Error::DEVICE_BUSY:		return _("Device busy");
    case Error::DEVICE_FORMAT:		return _("Failed to configure device format");
    case Error::DEVICE_BUFFER:		return _("Failed to configure device buffer");
    case Error::DEVICE_LATENCY:		return _("Failed to configure device latency");
    case Error::DEVICE_CHANNELS:	return _("Failed to configure number of device channels");
    case Error::DEVICE_FREQUENCY:	return _("Failed to configure device frequency");
    case Error::DEVICES_MISMATCH:	return _("Device configurations mismatch");
      // miscellaneous errors
    case Error::TEMP:			return _("Temporary error");
    case Error::WAVE_NOT_FOUND:		return _("No such wave");
    case Error::CODEC_FAILURE:		return _("Codec failure");
    case Error::UNIMPLEMENTED:		return _("Functionality not implemented");
    case Error::INVALID_PROPERTY:	return _("Invalid object property");
    case Error::INVALID_MIDI_CONTROL:	return _("Invalid MIDI control type");
    case Error::PARSE_ERROR:		return _("Parsing error");
    case Error::SPAWN:			return _("Failed to spawn child process");
    default:                            return "";
    }
}

// Map errno onto Ase::Error.
Error
ase_error_from_errno (int sys_errno, Error fallback)
{
  switch (sys_errno)
    {
    case 0:             return Error::NONE;
    case ELOOP:
    case ENAMETOOLONG:
    case ENOENT:        return Error::FILE_NOT_FOUND;
    case EISDIR:        return Error::FILE_IS_DIR;
    case EROFS:
    case EPERM:
    case EACCES:        return Error::PERMS;
#ifdef ENODATA  /* GNU/kFreeBSD lacks this */
    case ENODATA:
#endif
    case ENOMSG:        return Error::FILE_EOF;
    case ENOMEM:        return Error::NO_MEMORY;
    case ENOSPC:        return Error::NO_SPACE;
    case ENFILE:        return Error::NO_FILES;
    case EMFILE:        return Error::MANY_FILES;
    case EFBIG:
    case ESPIPE:
    case EIO:           return Error::IO;
    case EEXIST:        return Error::FILE_EXISTS;
    case ETXTBSY:
    case EBUSY:         return Error::FILE_BUSY;
    case EAGAIN:
    case EINTR:         return Error::TEMP;
    case EFAULT:        return Error::INTERNAL;
    case EBADF:
    case ENOTDIR:
    case ENODEV:
    case EINVAL:
    default:            return fallback;
    }
}

} // Ase
