module Id (X : sig type t end) = X
module A = struct type t end
module F (Y : sig type u = Id (A).t end) = struct end
