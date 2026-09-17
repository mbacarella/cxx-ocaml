module Id (X : sig type t end) = X
module G (X : sig type t end) = struct
  type u = Id (X).t
  type v = Id (X).t
  type w = Id (X).t
end
