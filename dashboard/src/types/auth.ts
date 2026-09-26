export type UserRole = 'VIEWER' | 'OPERATOR'

export interface AuthUser {
  username: string
  role: UserRole
}

export interface AuthSession {
  authenticated: true
  user: AuthUser
  csrfToken: string
  expiresAt: string
}

export interface AuthLoginResponse extends AuthSession {}

export interface AuthErrorPayload {
  error: string
}
