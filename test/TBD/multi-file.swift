// RUN: %empty-directory(%t)

// -Onone, non-resilient

// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-library -o %t/JustForTBDValidation %s %S/Inputs/multi-file2.swift -wmo -Xfrontend -validate-tbd-against-ir=all
// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-library -o %t/JustForTBDValidation %s %S/Inputs/multi-file2.swift -Xfrontend -validate-tbd-against-ir=all

// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-tbd-path %t/TBD.tbd -emit-module-path %t/multifile.swiftmodule %s %S/Inputs/multi-file2.swift -wmo
// RUN: diff %t/TBD.tbd %S/Inputs/multi-file-nonresilient-expected.tbd
// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-tbd-path %t/TBD.tbd -emit-module-path %t/multifile.swiftmodule %s %S/Inputs/multi-file2.swift
// RUN: diff %t/TBD.tbd %S/Inputs/multi-file-nonresilient-expected.tbd

// -O, non-resilient

// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-library -o %t/JustForTBDValidation %s %S/Inputs/multi-file2.swift -wmo -O -Xfrontend -validate-tbd-against-ir=all
// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-library -o %t/JustForTBDValidation %s %S/Inputs/multi-file2.swift -O -Xfrontend -validate-tbd-against-ir=all

// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-tbd-path %t/TBD.tbd -emit-module-path %t/multifile.swiftmodule %s %S/Inputs/multi-file2.swift -wmo -O
// RUN: diff %t/TBD.tbd %S/Inputs/multi-file-nonresilient-expected.tbd
// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-tbd-path %t/TBD.tbd -emit-module-path %t/multifile.swiftmodule %s %S/Inputs/multi-file2.swift -O
// RUN: diff %t/TBD.tbd %S/Inputs/multi-file-nonresilient-expected.tbd

// -Onone, resilient

// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-library -o %t/JustForTBDValidation %s %S/Inputs/multi-file2.swift -wmo -Xfrontend -enable-resilience -Xfrontend -validate-tbd-against-ir=all
// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-library -o %t/JustForTBDValidation %s %S/Inputs/multi-file2.swift -Xfrontend -enable-resilience -Xfrontend -validate-tbd-against-ir=all

// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-tbd-path %t/TBD.tbd -emit-module-path %t/multifile.swiftmodule %s %S/Inputs/multi-file2.swift -wmo -Xfrontend -enable-resilience
// RUN: diff %t/TBD.tbd %S/Inputs/multi-file-resilient-expected.tbd
// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-tbd-path %t/TBD.tbd -emit-module-path %t/multifile.swiftmodule %s %S/Inputs/multi-file2.swift -Xfrontend -enable-resilience
// RUN: diff %t/TBD.tbd %S/Inputs/multi-file-resilient-expected.tbd

// -O, resilient

// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-library -o %t/JustForTBDValidation %s %S/Inputs/multi-file2.swift -wmo -O -Xfrontend -enable-resilience -Xfrontend -validate-tbd-against-ir=all
// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-library -o %t/JustForTBDValidation %s %S/Inputs/multi-file2.swift -O -Xfrontend -enable-resilience -Xfrontend -validate-tbd-against-ir=all

// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-tbd-path %t/TBD.tbd -emit-module-path %t/multifile.swiftmodule %s %S/Inputs/multi-file2.swift -wmo -O -Xfrontend -enable-resilience
// RUN: diff %t/TBD.tbd %S/Inputs/multi-file-resilient-expected.tbd
// RUN: %target-build-swift -swift-version 4 -module-name multifile -emit-tbd-path %t/TBD.tbd -emit-module-path %t/multifile.swiftmodule %s %S/Inputs/multi-file2.swift -O -Xfrontend -enable-resilience
// RUN: diff %t/TBD.tbd %S/Inputs/multi-file-resilient-expected.tbd

// REQUIRES: objc_interop

public func function() {}

public class Class {
    public var property: Int

    public init() {
        property = 0
    }

    public static func staticFunc(default_: Int = 0) {}
}
