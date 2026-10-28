#pragma once

namespace pluginrack {

enum class SourceKind { Hardware, SystemLoopback, VirtualInput };
enum class OutputKind { Physical, VirtualOutput };
enum class ChannelLayout { Mono, Stereo };

} // namespace pluginrack
