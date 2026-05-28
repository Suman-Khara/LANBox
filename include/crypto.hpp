#pragma once
#include <string>
#include <vector>
#include <memory>

using namespace std;

// Forward declarations to avoid including OpenSSL headers here
typedef struct rsa_st RSA;
typedef struct evp_pkey_st EVP_PKEY;

class Crypto {
private:
    static inline string keyDir = "../keys/";
    static inline string privKeyFile = keyDir + "private.pem";
    static inline string pubKeyFile = keyDir + "public.pem";

public:
    // Existing functions...
    static bool generateKeyPair(int bits = 2048);
    static bool keysExist();
    static string loadPublicKey();
    static string loadPrivateKey();
    static string getPublicKeyFingerprint();
    static void ensureKeyDir();
    static string getKeyDir() { return keyDir; }
    
    // Digital signatures
    static vector<unsigned char> signMessage(const string& message);
    static bool verifySignature(const string& message, 
                               const vector<unsigned char>& signature,
                               const string& public_key_pem);
    
    // Helpers
    static string toHex(const vector<unsigned char>& data);
    static vector<unsigned char> fromHex(const string& hex);
    
    // AES Encryption/Decryption
    static vector<unsigned char> generateAESKey(int bits = 256);
    static vector<unsigned char> generateIV();  // Initialization Vector
    
    static bool encryptFileAES(const string& input_file,
                              const string& output_file,
                              const vector<unsigned char>& aes_key,
                              const vector<unsigned char>& iv);
    
    static bool decryptFileAES(const string& input_file,
                              const string& output_file,
                              const vector<unsigned char>& aes_key,
                              const vector<unsigned char>& iv);
    
    // RSA Encryption/Decryption (for encrypting the AES key)
    static vector<unsigned char> encryptRSA(const vector<unsigned char>& data,
                                           const string& public_key_pem);
    
    static vector<unsigned char> decryptRSA(const vector<unsigned char>& encrypted_data);
    
    // Hybrid Encryption (all-in-one)
    struct EncryptedFileInfo {
        vector<unsigned char> encrypted_aes_key;  // AES key encrypted with RSA
        vector<unsigned char> iv;                 // AES initialization vector
        string encrypted_file_path;               // Path to encrypted file
    };
    
    static EncryptedFileInfo encryptFile(const string& input_file,
                                        const string& output_file,
                                        const string& recipient_public_key);
    
    static bool decryptFile(const string& encrypted_file,
                           const string& output_file,
                           const vector<unsigned char>& encrypted_aes_key,
                           const vector<unsigned char>& iv);
};