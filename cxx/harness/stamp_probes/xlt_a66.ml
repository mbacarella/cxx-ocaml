module Id (X : sig type t end) = struct module M = struct type t = X.t end end
module A = struct type t end
type u = Id (A).M.t
