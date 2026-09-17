module Id (X : sig type t end) = X
module A = struct type t end
module F (Y : sig type t end) = struct type v = Id (Y).t end
