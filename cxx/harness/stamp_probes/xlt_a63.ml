module Id (X : sig type t end) = struct type t end
module A = struct type t end
type u = Id (A).t
