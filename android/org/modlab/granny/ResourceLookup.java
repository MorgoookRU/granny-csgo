package org.modlab.granny;

import android.content.res.Resources;

/** The cloned app and its unchanged compiled resources have different names. */
public final class ResourceLookup {
    private ResourceLookup() { }
    public static int identifier(Resources resources, String name, String type, String packageName) {
        int id = resources.getIdentifier(name,type,packageName);
        if (id != 0 || !"com.modlab.grannycsgo".equals(packageName)) return id;
        return resources.getIdentifier(name,type,"com.dvloper.granny");
    }
}
