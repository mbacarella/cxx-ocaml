module Id (X : sig type t end) (Y : sig type s end) = struct type t = X.t type
  s = Y.s end
module A = struct type t end
module B = struct type s end
type u = Id (A) (B).t
