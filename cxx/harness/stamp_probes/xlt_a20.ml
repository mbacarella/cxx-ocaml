module Id (X : sig type t end) = struct include X end
module A = struct type t end
type u = Id (A).t
