module F (X : sig module N : sig type t val v : t end end) = struct
  let v x = X.N.v = x
end
