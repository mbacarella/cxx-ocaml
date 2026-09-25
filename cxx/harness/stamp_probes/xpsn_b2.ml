module N = struct let v = "s" end
module F (X : sig module N : sig val v : int end end) = struct
  let v = X.N.v
  let w = N.v
end
