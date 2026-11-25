/// This file is a part of media_kit (https://github.com/media-kit/media-kit).
///
/// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
/// All rights reserved.
/// Use of this source code is governed by MIT license that can be found in the LICENSE file.
import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'package:path/path.dart' as path;
import 'package:safe_local_storage/safe_local_storage.dart';
import 'package:synchronized/synchronized.dart';

import 'package:media_kit/ffi/src/allocation.dart';
import 'package:media_kit/src/player/native/utils/temp_file.dart';
import 'package:media_kit/src/values.dart';

/// Callback invoked to notify about the released references.
typedef NativeReferenceHolderCallback = void Function(List<Pointer<Void>>);

/// {@template native_reference_holder}
///
/// NativeReferenceHolder
/// ---------------------
/// Holds references to [Pointer<generated.mpv_handle>]s created during the application runtime, while running in debug mode.
/// These references can be used to dispose the [Pointer<generated.mpv_handle>]s when they are no longer needed i.e. upon hot-restart.
///
/// {@endtemplate}
class NativeReferenceHolder {
  /// Maximum number of references that can be held.
  static const int kReferenceBufferSize = 512;

  /// Singleton instance.
  static final NativeReferenceHolder instance = NativeReferenceHolder._();

  /// Whether the [instance] is initialized.
  static bool initialized = false;

  /// {@macro native_reference_holder}
  NativeReferenceHolder._();

  /// Initializes the instance.
  static void ensureInitialized(NativeReferenceHolderCallback callback) {
    if (!kDebugMode) return;
    if (initialized) return;
    initialized = true;
    instance._ensureInitialized(callback);
  }

  void _ensureInitialized(NativeReferenceHolderCallback callback) async {
    if (!await _file.exists_()) {
      // Allocate reference buffer.
      _referenceBuffer = calloc<IntPtr>(kReferenceBufferSize);
      final address = _referenceBuffer.address;
      await _file.write_(address.toString());
      print('$kTag Allocated $address');
      await _deleteStaleFiles();
    } else {
      // Locate reference buffer.
      final address = int.parse((await _file.readAsString_())!);
      _referenceBuffer = Pointer<IntPtr>.fromAddress(address);
      print('$kTag Located $address');
    }

    final references = <Pointer<Void>>[];

    for (int i = 0; i < kReferenceBufferSize; i++) {
      final referencePtr = _referenceBuffer + i;
      final referenceAddress = referencePtr.value;
      referencePtr.value = 0;
      if (referenceAddress != 0) {
        references.add(Pointer.fromAddress(referenceAddress));
      }
    }

    callback(references);

    _completer.complete();
  }

  /// Saves the reference.
  Future<void> add(Pointer reference) async {
    if (!initialized) return;
    if (reference == nullptr) return;
    await _completer.future;
    return _lock.synchronized(() async {
      for (int i = 0; i < kReferenceBufferSize; i++) {
        final referenceValue = _referenceBuffer + i;
        final referencePtr = Pointer.fromAddress(referenceValue.value);
        // NOTE: Do not compare .value with .address. Bad things may happen on 32-bit systems.
        if (referencePtr.address == 0) {
          referenceValue.value = reference.address;
          break;
        }
      }
    });
  }

  /// Removes the reference.
  Future<void> remove(Pointer reference) async {
    if (!initialized) return;
    if (reference == nullptr) return;
    await _completer.future;
    return _lock.synchronized(() async {
      for (int i = 0; i < kReferenceBufferSize; i++) {
        final referenceValue = _referenceBuffer + i;
        final referencePtr = Pointer.fromAddress(referenceValue.value);
        // NOTE: Do not compare .value with .address. Bad things may happen on 32-bit systems.
        if (referencePtr.address == reference.address) {
          referenceValue.value = 0;
          break;
        }
      }
    });
  }

  /// [Lock] used to synchronize access to the reference buffer.
  final Lock _lock = Lock();

  /// [Completer] used to wait for the reference buffer to be allocated.
  final Completer<void> _completer = Completer<void>();

  /// [File] used to store [int] address to the reference buffer.
  /// This is necessary to have a persistent to the reference buffer across hot-restarts.
  ///
  /// The file outlives the process (on Android it sits in the app's files
  /// directory), so its name carries the process start time where procfs
  /// gives one. Keyed by the PID alone, a later process that reused the PID
  /// read a dead process's address and crashed.
  final File _file = File(
    path.join(TempFile.directory, '$kFilePrefix${processKey(pid)}'),
  );

  /// Removes files left by processes that have ended, which otherwise pile up
  /// with every launch. Only where procfs can tell a live process apart.
  Future<void> _deleteStaleFiles() async {
    if (!File('/proc/self/stat').existsSync()) return;
    final own = path.basename(_file.path);
    try {
      await for (final entity in Directory(TempFile.directory).list()) {
        final name = path.basename(entity.path);
        if (!name.startsWith(kFilePrefix) || name == own) continue;
        final key = name.substring(kFilePrefix.length);
        final owner = int.tryParse(key.split('.').first);
        if (owner == null) continue;
        // A bare PID may be a live process on an older media_kit, so only an
        // ended one goes. A keyed file also goes once its PID was reused.
        if (!_isAlive(owner) ||
            (key.contains('.') && processKey(owner) != key)) {
          try {
            await entity.delete();
          } catch (_) {}
        }
      }
    } catch (_) {}
  }

  /// Android refuses the lookup for another user's process rather than
  /// answering, which still means one is running under that PID.
  static bool _isAlive(int pid) {
    try {
      return Directory('/proc/$pid').existsSync();
    } on FileSystemException {
      return true;
    }
  }

  /// [pid] with its start time and boot from procfs, so a hot-restart in the
  /// same process finds its own file and a new process with a reused PID
  /// doesn't. Just [pid] where procfs is missing or the process has ended.
  static String processKey(int pid) {
    try {
      return processKeyFromProcfs(
            pid: pid,
            stat: File('/proc/$pid/stat').readAsStringSync(),
            boot: _boot,
          ) ??
          '$pid';
    } catch (_) {
      return '$pid';
    }
  }

  /// Tells boots apart, since a start time counts from boot. Android keeps
  /// `/proc/stat` from apps, so it falls back to the kernel's boot id there.
  static final String _boot = () {
    final reads = <String Function()>[
      () => File('/proc/stat')
          .readAsLinesSync()
          .firstWhere((line) => line.startsWith('btime '))
          .substring(6)
          .trim(),
      () => File('/proc/sys/kernel/random/boot_id').readAsStringSync().trim(),
    ];
    for (final read in reads) {
      try {
        return read();
      } catch (_) {}
    }
    return '';
  }();

  /// Builds the key from the contents of `/proc/<pid>/stat` and [boot].
  static String? processKeyFromProcfs({
    required int pid,
    required String stat,
    required String boot,
  }) {
    // The process name in parentheses can hold spaces, so fields are counted
    // from after its closing parenthesis, which leaves field 3 (state) first
    // and field 22 (starttime) at index 19.
    final fields = stat.substring(stat.lastIndexOf(')') + 2).split(' ');
    if (fields.length < 20) return null;
    return [pid, fields[19], if (boot.isNotEmpty) boot].join('.');
  }

  static const String kFilePrefix =
      'com.alexmercerind.media_kit.NativeReferenceHolder.';

  /// [Pointer] to the reference buffer.
  late final Pointer<IntPtr> _referenceBuffer;

  static const String kTag = 'media_kit: NativeReferenceHolder:';
}
