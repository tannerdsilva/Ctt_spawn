import XCTest
@testable import Cclone

final class CcloneTests: XCTestCase {
    func testExample() {
        // This is an example of a functional test case.
        // Use XCTAssert and related functions to verify your tests produce the correct
        // results.
        XCTAssertEqual(Cclone().text, "Hello, World!")
    }

    static var allTests = [
        ("testExample", testExample),
    ]
}
