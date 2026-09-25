module F (X : sig module N : sig type t val v : t -> t end end) = struct
  let v = X.N.v
end
