module Id (X : sig type t end) = X
module G (X : sig type t end) : sig type u end = struct
  type u = Id (X).t
end
