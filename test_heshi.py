"""Тесты хешей и кодов подлинности: хеш_sha224, хеш_sha256, хеш_sha384, хеш_sha512,
хеш_sha3_256, хеш_sha3_512, хеш_blake3, hmac_sha256, hmac_sha512, hkdf_sha256,
pbkdf2_sha256, равны_за_постоянное_время.

Эталоны: примеры NIST для SHA-2 и SHA-3 (FIPS 180-4, FIPS 202: "", "abc", 448 и 896 бит,
миллион букв «a»), RFC 4231 (HMAC-SHA256 и HMAC-SHA512, случаи 1-7), RFC 5869 (HKDF-SHA256,
случаи 1-3), RFC 7914 раздел 11 и общеизвестные векторы PBKDF2-HMAC-SHA256 в виде RFC 6070,
официальные векторы BLAKE3 (test_vectors.json, обычный хеш: первые 32 байта). Кроме того -
сверка с hashlib и hmac Python на случайных данных длиной 0, 1, 55, 56, 63, 64, 65, 127, 128
и до 1 МБ (BLAKE3 - с модулем blake3, если он установлен).
"""

import hashlib
import hmac
import os
import random
import shutil
import subprocess
import tempfile
import unittest

import dan

КОМПИЛЯТОР_ЕСТЬ = dan.компилятор_есть()

try:
    import blake3 as blake3_python
except ImportError:
    blake3_python = None

ХЕШИ = (("хеш_sha224", "sha224"), ("хеш_sha256", "sha256"), ("хеш_sha384", "sha384"),
        ("хеш_sha512", "sha512"), ("хеш_sha3_256", "sha3_256"), ("хеш_sha3_512", "sha3_512"))

# FIPS 180-4 и FIPS 202 (примеры NIST): sha224, sha256, sha384, sha512, sha3_256, sha3_512
NIST = [
    ('""', [
        "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b",
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f"
        "63b931bd47417a81a538327af927da3e",
        "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
        "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c11e3e9402c3ac558"
        "f500199d95b6d3e301758586281dcd26"]),
    ('"abc"', [
        "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
        "454d4423643ce80e2a9ac94fa54ca49f",
        "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
        "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e39340"
        "57340b4cf408d5a56592f8274eec53f0"]),
    ('"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"', [
        "75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "3391fdddfc8dc7393707a65b1b4709397cf8b1d162af05abfe8f450de5f36bc6b0455a8520bc4e6f5fe95b1fe3c8452b",
        "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c33596fd15c13b1b07f9aa1d3bea57789ca0"
        "31ad85c7a71dd70354ec631238ca3445",
        "41c0dba2a9d6240849100376a8235e2c82e1b9998a999e21db32dd97496d3376",
        "04a371e84ecfb5b8b77cb48610fca8182dd457ce6f326a0fd3d7ec2f1e91636dee691fbe0c985302ba1b0d8dc78c0863"
        "46b533b49c030d99a27daf1139d6e75e"]),
    ('"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs'
     'mnopqrstnopqrstu"', [
        "c97ca9a559850ce97a04a96def6d99a9e0e0e2ab14e6b8df265fc0b3",
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
        "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039",
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433a"
        "c7d329eeb6dd26545e96e55b874be909",
        "916f6061fe879741ca6469b43971dfdb28b1a32dc36cb3254e812be27aad1d18",
        "afebb2ef542e6579c50cad06d2e578f9f8dd6881d7dc824d26360feebf18a4fa73e3261122948efcfd492e74e82e2189"
        "ed0fb440d187f382270cb455f21dd185"]),
    ('"a".повторить(1000000)', [
        "20794655980c91d8bbb4c1ea97618a4bf03f42581948b2ee4ee7ad67",
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b07b8b3dc38ecc4ebae97ddd87f3d8985",
        "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31b"
        "eb009c5c2c49aa2e4eadb217ad8cc09b",
        "5c8875ae474a3634ba4fd55ec85bffd661f32aca75c6d699d0cdcb6c115891c1",
        "3c3a876da14034ab60627c077bb98f7e120a2a5370212dffb3385a18d4f38859ed311d0a9d5141ce9cc5c66ee689b266"
        "a8aa18ace8282a0e0db596c90b0a7b87"]),
]

# официальные векторы BLAKE3: вход длины н из байтов 0, 1, ..., 250, 0, 1, ...
BLAKE3 = {
    0: "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
    1: "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213",
    2: "7b7015bb92cf0b318037702a6cdd81dee41224f734684c2c122cd6359cb1ee63",
    3: "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f",
    4: "f30f5ab28fe047904037f77b6da4fea1e27241c5d132638d8bedce9d40494f32",
    5: "b40b44dfd97e7a84a996a91af8b85188c66c126940ba7aad2e7ae6b385402aa2",
    6: "06c4e8ffb6872fad96f9aaca5eee1553eb62aed0ad7198cef42e87f6a616c844",
    7: "3f8770f387faad08faa9d8414e9f449ac68e6ff0417f673f602a646a891419fe",
    8: "2351207d04fc16ade43ccab08600939c7c1fa70a5c0aaca76063d04c3228eaeb",
    63: "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b",
    64: "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98",
    65: "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee",
    127: "d81293fda863f008c09e92fc382a81f5a0b4a1251cba1634016a0f86a6bd640d",
    128: "f17e570564b26578c33bb7f44643f539624b05df1a76c81f30acd548c44b45ef",
    129: "683aaae9f3c5ba37eaaf072aed0f9e30bac0865137bae68b1fde4ca2aebdcb12",
    1023: "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11",
    1024: "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7",
    1025: "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444",
    2048: "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a",
    2049: "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030",
    3072: "b98cb0ff3623be03326b373de6b9095218513e64f1ee2edd2525c7ad1e5cffd2",
    3073: "7124b49501012f81cc7f11ca069ec9226cecb8a2c850cfe644e327d22d3e1cd3",
    4096: "015094013f57a5277b59d8475c0501042c0b642e531b0a1c8f58d2163229e969",
    4097: "9b4052b38f1c5fc8b1f9ff7ac7b27cd242487b3d890d15c96a1c25b8aa0fb995",
    5120: "9cadc15fed8b5d854562b26a9536d9707cadeda9b143978f319ab34230535833",
    5121: "628bd2cb2004694adaab7bbd778a25df25c47b9d4155a55f8fbd79f2fe154cff",
    6144: "3e2e5b74e048f3add6d21faab3f83aa44d3b2278afb83b80b3c35164ebeca205",
    6145: "f1323a8631446cc50536a9f705ee5cb619424d46887f3c376c695b70e0f0507f",
    7168: "61da957ec2499a95d6b8023e2b0e604ec7f6b50e80a9678b89d2628e99ada77a",
    7169: "a003fc7a51754a9b3c7fae0367ab3d782dccf28855a03d435f8cfe74605e7817",
    8192: "aae792484c8efe4f19e2ca7d371d8c467ffb10748d8a5a1ae579948f718a2a63",
    8193: "bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b",
    16384: "f875d6646de28985646f34ee13be9a576fd515f76b5b0a26bb324735041ddde4",
    31744: "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47",
    102400: "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085",
}

# RFC 4231, случаи 1-7: ключ, данные (шестнадцатеричные), HMAC-SHA256, HMAC-SHA512;
# в случае 5 RFC даёт только первые 128 бит
_AA131 = "aa" * 131
RFC4231 = [
    ("0b" * 20, "4869205468657265",
     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
     "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
     "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"),
    ("4a656665", "7768617420646f2079612077616e7420666f72206e6f7468696e673f",
     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
     "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
     "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"),
    ("aa" * 20, "dd" * 50,
     "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
     "fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39"
     "bf3e848279a722c806b485a47e67c807b946a337bee8942674278859e13292fb"),
    ("0102030405060708090a0b0c0d0e0f10111213141516171819", "cd" * 50,
     "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
     "b0ba465637458c6990e5a8c5f61d4af7e576d97ff94b872de76f8050361ee3db"
     "a91ca5c11aa25eb4d679275cc5788063a5f19741120c4f2de2adebeb10a298dd"),
    ("0c" * 20, "546573742057697468205472756e636174696f6e",
     "a3b6167473100ee06e0c796c2955552b", "415fad6271580a531d4179bc891d87a6"),
    (_AA131, "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d20"
             "48617368204b6579204669727374",
     "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
     "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f352"
     "6b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598"),
    (_AA131, "5468697320697320612074657374207573696e672061206c6172676572207468616e20626c6f636b2d"
             "73697a65206b657920616e642061206c6172676572207468616e20626c6f636b2d73697a6520646174"
             "612e20546865206b6579206e6565647320746f20626520686173686564206265666f7265206265696e"
             "6720757365642062792074686520484d414320616c676f726974686d2e",
     "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
     "e37b6a775dc87dbaa4dfa9f96e5e3ffddebd71f8867289865df5a32d20cdc944"
     "b6022cac3c4982b10d5eeb55c3e4de15134676fb6de0446065c97440fa8c6a58"),
]

# RFC 5869, случаи 1-3 (SHA-256): IKM, соль, сведения, длина, OKM
RFC5869 = [
    ("0b" * 22, "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9", 42,
     "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"),
    (bytes(range(0x50)).hex(), bytes(range(0x60, 0xb0)).hex(), bytes(range(0xb0, 0x100)).hex(), 82,
     "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09"
     "da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"),
    ("0b" * 22, "", "", 42,
     "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"),
]

# PBKDF2-HMAC-SHA256: RFC 7914 раздел 11 (первые два) и векторы в виде RFC 6070 для SHA-256
PBKDF2 = [
    (b"passwd", b"salt", 1, 64,
     "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
     "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"),
    (b"Password", b"NaCl", 80000, 64,
     "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
     "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"),
    (b"password", b"salt", 1, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"),
    (b"password", b"salt", 2, 32, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"),
    (b"password", b"salt", 4096, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"),
    (b"passwordPASSWORDpassword", b"saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 40,
     "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9"),
    (b"pass\0word", b"sa\0lt", 4096, 16, "89b69d0516f829893c696226650a8687"),
]


def _б(шест):
    return f'из_шестнадцатеричной("{шест}")'


@unittest.skipUnless(КОМПИЛЯТОР_ЕСТЬ, "нет компилятора C")
class ТестыХешей(unittest.TestCase):

    def setUp(self):
        self.папка = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.папка, ignore_errors=True)

    def запустить(self, текст):
        путь = os.path.join(self.папка, "программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        исполняемый = dan.собрать(путь, текст)
        р = subprocess.run([исполняемый], capture_output=True, text=True, encoding="utf-8",
                           timeout=300, cwd=self.папка)
        return р.returncode, р.stdout, р.stderr

    def строки(self, текст):
        код, вывод, ошибки = self.запустить(текст)
        self.assertEqual(код, 0, ошибки)
        return вывод.splitlines()

    def test_эталоны(self):
        """Примеры NIST, RFC 4231, RFC 5869, векторы PBKDF2 - одной программой."""
        ш = "шестнадцатеричная"
        т, ждём = [], []
        for вход, хеши in NIST:
            т.append(f"пусть в{len(т)} = {вход}")
            имя = f"в{len(т) - 1}"
            for (функция, _), хеш in zip(ХЕШИ, хеши):
                т.append(f"вывод({ш}({функция}({имя})))")
                т.append(f"вывод({ш}({функция}(байты({имя}))))")
                ждём += [хеш, хеш]
            т.append(f"вывод({ш}(хеш_sha256({имя})) == хеш({имя}))")
            ждём.append("истина")
        for ключ, данные, h256, h512 in RFC4231:
            т.append(f"вывод({ш}(hmac_sha256({_б(ключ)}, {_б(данные)})))")
            т.append(f"вывод({ш}(hmac_sha512({_б(ключ)}, {_б(данные)})))")
            ждём += [h256, h512]
        for ikm, соль, сведения, длина, okm in RFC5869:
            т.append(f"вывод({ш}(hkdf_sha256({_б(ikm)}, {_б(соль)}, {_б(сведения)}, {длина})))")
            ждём.append(okm)
        for пароль, соль, повторов, длина, ключ in PBKDF2:
            т.append(f"вывод({ш}(pbkdf2_sha256({_б(пароль.hex())}, {_б(соль.hex())}, {повторов}, {длина})))")
            ждём.append(ключ)
            if b"\0" not in пароль:
                т.append(f'вывод({ш}(pbkdf2_sha256("{пароль.decode()}", {_б(соль.hex())}, {повторов}, {длина})))')
                ждём.append(ключ)
        # строки и байты в разных сочетаниях, кириллица - как UTF-8
        ключ, данные = "ключ", "данные для подписи"
        т += [f'вывод({ш}(hmac_sha256("{ключ}", "{данные}")))',
              f'вывод({ш}(hmac_sha256(байты("{ключ}"), "{данные}")))',
              f'вывод({ш}(hmac_sha512("{ключ}", байты("{данные}"))))',
              f'вывод({ш}(hkdf_sha256(байты("{ключ}"), байты("соль"), "сведения", 100)))']
        кб, дб = ключ.encode(), данные.encode()
        prk = hmac.new("соль".encode(), кб, "sha256").digest()
        okm, t_ = b"", b""
        for i in range(1, 5):
            t_ = hmac.new(prk, t_ + "сведения".encode() + bytes([i]), "sha256").digest()
            okm += t_
        ждём += [hmac.new(кб, дб, "sha256").hexdigest(), hmac.new(кб, дб, "sha256").hexdigest(),
                 hmac.new(кб, дб, "sha512").hexdigest(), okm[:100].hex()]
        вывод = self.строки("\n".join(т) + "\n")
        self.assertEqual(len(вывод), len(ждём))
        for н, (получено, ожидается) in enumerate(zip(вывод, ждём)):
            # RFC 4231 случай 5 - только первые 128 бит
            self.assertEqual(получено[:len(ожидается)], ожидается, f"строка {н + 1}: {т[н] if н < len(т) else ''}")

    def test_случайные_данные_и_blake3(self):
        """Файлы разной длины: SHA-2, SHA-3, HMAC сверяются с hashlib и hmac, BLAKE3 - с официальными
        векторами (и модулем blake3, если он есть)."""
        ген = random.Random(20261111)
        входы = {}
        for н in (0, 1, 2, 55, 56, 57, 63, 64, 65, 71, 72, 73, 111, 112, 113, 127, 128, 129, 135, 136, 137,
                  200, 1000, 1023, 1024, 1025, 4097, 65536, 1048576):
            входы[f"с{н:08d}"] = bytes(ген.getrandbits(8) for _ in range(н)) if н < 70000 else os.urandom(н)
        for н in BLAKE3:
            входы[f"э{н:08d}"] = bytes(к % 251 for к in range(н))
        os.mkdir(os.path.join(self.папка, "входы"))
        for имя, данные in входы.items():
            with open(os.path.join(self.папка, "входы", имя), "wb") as ф:
                ф.write(данные)
        ключ = bytes(ген.getrandbits(8) for _ in range(40))
        текст = (f'пусть ключ = из_шестнадцатеричной("{ключ.hex()}")\n'
                 'для имя в список_файлов("входы") {\n'
                 '    пусть д = прочитать_байты("входы/" + имя)\n'
                 '    перем ч: [стр] = [имя]\n'
                 + "".join(f'    ч.добавить(шестнадцатеричная({ф}(д)))\n' for ф, _ in ХЕШИ) +
                 '    ч.добавить(шестнадцатеричная(хеш_blake3(д)))\n'
                 '    ч.добавить(шестнадцатеричная(hmac_sha256(ключ, д)))\n'
                 '    ч.добавить(шестнадцатеричная(hmac_sha512(ключ, д)))\n'
                 '    ч.добавить(шестнадцатеричная(hmac_sha256(д, "сообщение")))\n'
                 '    ч.добавить(шестнадцатеричная(hmac_sha512(д, "сообщение")))\n'
                 '    вывод(ч.соединить(" "))\n'
                 '}\n')
        путь = os.path.join(self.папка, "программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        исполняемый = dan.собрать(путь, текст)
        выводы = []
        # SHA-256 командами процессора (SHA-NI), если они есть, и кодом на C (DAN_SHA=c)
        for среда in ({}, {"DAN_SHA": "c"}):
            р = subprocess.run([исполняемый], capture_output=True, text=True, encoding="utf-8", timeout=300,
                               cwd=self.папка, env=dict(os.environ, **среда))
            self.assertEqual(р.returncode, 0, р.stderr)
            выводы.append(р.stdout)
        self.assertEqual(выводы[0], выводы[1])
        вывод = выводы[0].splitlines()
        self.assertEqual(len(вывод), len(входы))
        сообщение = "сообщение".encode()
        for строка in вывод:
            имя, *части = строка.split(" ")
            д = входы[имя]
            ждём = [hashlib.new(х, д).hexdigest() for _, х in ХЕШИ]
            ждём.append(BLAKE3[len(д)] if имя.startswith("э")
                        else blake3_python.blake3(д).hexdigest() if blake3_python else части[6])
            ждём += [hmac.new(ключ, д, "sha256").hexdigest(), hmac.new(ключ, д, "sha512").hexdigest(),
                     hmac.new(д, сообщение, "sha256").hexdigest(), hmac.new(д, сообщение, "sha512").hexdigest()]
            self.assertEqual(части, ждём, f"вход {имя}, длина {len(д)}")

    def test_сравнение_за_постоянное_время(self):
        self.assertEqual(self.строки(
            'пусть а = хеш_sha256("x")\n'
            'вывод(равны_за_постоянное_время(а, хеш_sha256("x")))\n'
            'вывод(равны_за_постоянное_время(а, хеш_sha256("y")))\n'
            'вывод(равны_за_постоянное_время(а, а[0..30]))\n'
            'вывод(равны_за_постоянное_время(байты(""), байты("")))\n'
            'вывод(равны_за_постоянное_время(байты("абв"), байты("абг")))\n'
            'вывод(равны_за_постоянное_время("код", "код"))\n'
            'вывод(равны_за_постоянное_время("код", "кот"))\n'),
            ["истина", "ложь", "ложь", "истина", "ложь", "истина", "ложь"])

    def test_пределы(self):
        """Неверная длина ключа или число повторов - ошибка выполнения с понятным сообщением."""
        случаи = [
            ('hkdf_sha256(байты("к"), байты(""), "", 0)', "от 1 до 8160"),
            ('hkdf_sha256(байты("к"), байты(""), байты(""), 8161)', "от 1 до 8160"),
            ('pbkdf2_sha256("п", байты("с"), 0, 32)', "от 1 до 100000000"),
            ('pbkdf2_sha256("п", байты("с"), 100000001, 32)', "от 1 до 100000000"),
            ('pbkdf2_sha256(байты("п"), байты("с"), 1, 0)', "от 1 до 1024"),
            ('pbkdf2_sha256(байты("п"), байты("с"), 1, 1025)', "от 1 до 1024"),
        ]
        # одна программа: номер случая берётся из файла
        текст = ("пусть н = цел(прочитать(\"н.txt\"))\n"
                 + "".join(f"если н == {к} {{ вывод({в}.длина) }}\n" for к, (в, _) in enumerate(случаи))
                 + 'вывод(hkdf_sha256(байты("к"), байты(""), "", 8160).длина)\n'
                 + 'вывод(pbkdf2_sha256("п", байты(""), 1, 1024).длина)\n')
        путь = os.path.join(self.папка, "программа.dan")
        with open(путь, "w", encoding="utf-8") as ф:
            ф.write(текст)
        исполняемый = dan.собрать(путь, текст)
        for к, (_, сообщение) in enumerate(случаи + [(None, None)]):
            with open(os.path.join(self.папка, "н.txt"), "w") as ф:
                ф.write(str(к))
            р = subprocess.run([исполняемый], capture_output=True, text=True, encoding="utf-8",
                               timeout=60, cwd=self.папка)
            if сообщение is None:
                self.assertEqual((р.returncode, р.stdout.split()), (0, ["8160", "1024"]), р.stderr)
            else:
                self.assertNotEqual(р.returncode, 0)
                self.assertIn(сообщение, р.stderr)


if __name__ == "__main__":
    unittest.main()
