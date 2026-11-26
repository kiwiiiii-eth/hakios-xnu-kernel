// aea-key: the key of an encrypted disk image of an IPSW (NAME.dmg.aea, Darwin userland 18 and later), for /usr/bin/aea.
//
// The archive says where its key is: its header carries "com.apple.wkms.fcs-key-url", the address of a private key that
// Apple publishes, and "com.apple.wkms.fcs-response", the archive's own key wrapped for that private key with HPKE
// (RFC 9180: P-256, HKDF-SHA256, AES-256-GCM). This fetches the one and unwraps the other, and prints the key as
// base64 -- nothing else, so that it can be used like this:
//
//   aea decrypt -i NAME.dmg.aea -o NAME.dmg -key-value "base64:$(swift sysimage/aea-key.swift NAME.dmg.aea)"
//
// Needs macOS 14 (CryptoKit's HPKE) and the network, for the one small file from wkms-public.apple.com.
import CryptoKit
import Foundation

func fail(_ what: String) -> Never {
	FileHandle.standardError.write(("aea-key: " + what + "\n").data(using: .utf8)!)
	exit(1)
}

guard CommandLine.arguments.count == 2 else { fail("usage: aea-key.swift NAME.dmg.aea") }
guard let file = FileHandle(forReadingAtPath: CommandLine.arguments[1]) else { fail("cannot open \(CommandLine.arguments[1])") }
let head = file.readData(ofLength: 12)
guard head.count == 12, head.prefix(4) == Data("AEA1".utf8) else { fail("not an encrypted archive (no AEA1 at its start)") }
let authSize = Int(head[8]) | Int(head[9]) << 8 | Int(head[10]) << 16 | Int(head[11]) << 24
guard authSize > 0, authSize < 1 << 20 else { fail("the archive carries no description of its key") }
let auth = file.readData(ofLength: authSize)

// the header's entries: a length (four bytes, itself included), then "key", a zero byte, the value
var fields: [String: Data] = [:]
var at = 0
while at + 4 <= auth.count {
	let n = Int(auth[at]) | Int(auth[at + 1]) << 8 | Int(auth[at + 2]) << 16 | Int(auth[at + 3]) << 24
	guard n > 4, at + n <= auth.count else { break }
	let entry = auth.subdata(in: (at + 4)..<(at + n))
	if let zero = entry.firstIndex(of: 0), let key = String(data: entry.prefix(upTo: zero), encoding: .utf8) {
		fields[key] = entry.suffix(from: zero + 1)
	}
	at += n
}
guard let urlData = fields["com.apple.wkms.fcs-key-url"], let urlText = String(data: urlData, encoding: .utf8),
      let url = URL(string: urlText), url.scheme == "https", url.host == "wkms-public.apple.com" else {
	fail("no key address in the archive, or not one of wkms-public.apple.com")
}
guard let responseData = fields["com.apple.wkms.fcs-response"],
      let response = try? JSONSerialization.jsonObject(with: responseData) as? [String: String],
      let enc = Data(base64Encoded: response["enc-request"] ?? ""),
      let wrapped = Data(base64Encoded: response["wrapped-key"] ?? "") else {
	fail("the archive's wrapped key cannot be read")
}
guard let pemData = try? Data(contentsOf: url), let pem = String(data: pemData, encoding: .utf8) else {
	fail("cannot fetch \(urlText)")
}
do {
	let privateKey = try P256.KeyAgreement.PrivateKey(pemRepresentation: pem)
	var recipient = try HPKE.Recipient(privateKey: privateKey, ciphersuite: .P256_SHA256_AES_GCM_256, info: Data(),
	    encapsulatedKey: enc)
	let key = try recipient.open(wrapped)
	print(key.base64EncodedString())
} catch {
	fail("the key did not unwrap: \(error)")
}
