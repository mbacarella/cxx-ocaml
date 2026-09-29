module F (X : sig module N : sig module P : sig type t val v : t end end end) =
struct
  let v = X.N.P.v
end
