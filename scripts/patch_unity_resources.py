"""Route Unity's resource lookup through the clone's namespace fallback."""
import re

CALL = re.compile(r'invoke-virtual \{([^}]+)\}, Landroid/content/res/Resources;->getIdentifier\(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;\)I')
TARGET = 'Lorg/modlab/granny/ResourceLookup;->identifier(Landroid/content/res/Resources;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I'


def patch_unity_resources(decoded):
    changed = []
    total = 0
    for path in sorted(decoded.glob('smali*/com/unity3d/player/**/*.smali')):
        source = path.read_text()
        result, count = CALL.subn(lambda match: 'invoke-static {' + match.group(1) + '}, ' + TARGET, source)
        total += result.count(TARGET)
        if count:
            path.write_text(result)
            changed.append((str(path.relative_to(decoded)), count))
    if total < 7:
        raise ValueError('Expected all Unity resource lookups, including the SurfaceView description')
    return changed, total
