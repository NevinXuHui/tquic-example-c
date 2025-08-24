#!/bin/bash

# Generate self-signed certificates for QUIC WebSocket server
# This script creates a certificate authority and server certificate

set -e

CERT_DIR="certs"
CA_KEY="$CERT_DIR/ca-key.pem"
CA_CERT="$CERT_DIR/ca-cert.pem"
SERVER_KEY="$CERT_DIR/key.pem"
SERVER_CERT="$CERT_DIR/cert.pem"

echo "🔐 Generating certificates for QUIC WebSocket server..."

# Create certs directory
mkdir -p "$CERT_DIR"

# Generate CA private key
echo "📝 Generating CA private key..."
openssl genrsa -out "$CA_KEY" 4096

# Generate CA certificate
echo "📝 Generating CA certificate..."
openssl req -new -x509 -days 365 -key "$CA_KEY" -out "$CA_CERT" \
    -subj "/C=US/ST=CA/L=San Francisco/O=QUIC WebSocket/OU=Development/CN=QUIC WebSocket CA"

# Generate server private key in PKCS#8 format (required by rustls)
echo "📝 Generating server private key..."
openssl genpkey -algorithm RSA -out "$SERVER_KEY"

# Generate server certificate signing request
echo "📝 Generating server certificate signing request..."
openssl req -new -key "$SERVER_KEY" -out "$CERT_DIR/server.csr" \
    -subj "/C=US/ST=CA/L=San Francisco/O=QUIC WebSocket/OU=Development/CN=localhost"

# Create extensions file for server certificate
cat > "$CERT_DIR/server.ext" << EOF
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature, nonRepudiation, keyEncipherment, dataEncipherment
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
DNS.2 = *.localhost
IP.1 = 127.0.0.1
IP.2 = ::1
EOF

# Generate server certificate signed by CA
echo "📝 Generating server certificate..."
openssl x509 -req -in "$CERT_DIR/server.csr" -CA "$CA_CERT" -CAkey "$CA_KEY" \
    -CAcreateserial -out "$SERVER_CERT" -days 365 \
    -extfile "$CERT_DIR/server.ext"

# Clean up temporary files
rm "$CERT_DIR/server.csr" "$CERT_DIR/server.ext"

# Set appropriate permissions
chmod 600 "$CA_KEY" "$SERVER_KEY"
chmod 644 "$CA_CERT" "$SERVER_CERT"

echo "✅ Certificates generated successfully!"
echo ""
echo "📁 Generated files:"
echo "  CA Certificate:     $CA_CERT"
echo "  CA Private Key:     $CA_KEY"
echo "  Server Certificate: $SERVER_CERT"
echo "  Server Private Key: $SERVER_KEY"
echo ""
echo "🚀 You can now start the server with:"
echo "  cargo run --bin server -- --cert $SERVER_CERT --key $SERVER_KEY"
echo ""
echo "🔍 To verify the certificate:"
echo "  openssl x509 -in $SERVER_CERT -text -noout"
echo ""
echo "⚠️  Note: These are self-signed certificates for development only."
echo "   For production, use certificates from a trusted CA."
