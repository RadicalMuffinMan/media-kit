import 'package:test/test.dart';

import 'package:media_kit/src/player/native/utils/native_reference_holder.dart';

void main() {
  // A real /proc/<pid>/stat line, with a space and a parenthesis in the name.
  const stat = '4321 (flutter (ui) x) S 1 4321 4321 0 -1 4194560 2533 0 0 0 '
      '12 7 0 0 20 0 18 0 98765 123456789 1234 18446744073709551615';

  test('key carries the start time and boot', () {
    expect(
      NativeReferenceHolder.processKeyFromProcfs(
        pid: 4321,
        stat: stat,
        boot: '1762470000',
      ),
      '4321.98765.1762470000',
    );
  });

  test('a reused PID gets a different key', () {
    final later = stat.replaceFirst(' 98765 ', ' 99999 ');
    expect(
      NativeReferenceHolder.processKeyFromProcfs(pid: 4321, stat: later, boot: 'b'),
      isNot(NativeReferenceHolder.processKeyFromProcfs(pid: 4321, stat: stat, boot: 'b')),
    );
  });

  test('no boot still keys by start time', () {
    expect(
      NativeReferenceHolder.processKeyFromProcfs(pid: 4321, stat: stat, boot: ''),
      '4321.98765',
    );
  });

  test('a truncated stat line gives no key', () {
    expect(
      NativeReferenceHolder.processKeyFromProcfs(
        pid: 4321,
        stat: '4321 (x) S 1',
        boot: 'b',
      ),
      isNull,
    );
  });
}
