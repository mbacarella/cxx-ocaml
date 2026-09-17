module Id (X : sig type t end) = struct type t = X.t type s = X.t module M =
  struct type r end end
module A = struct type t end
type u = Id (A).t
