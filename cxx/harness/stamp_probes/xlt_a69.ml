module Id (X : sig type t end) = X
type u = Id (String).t
