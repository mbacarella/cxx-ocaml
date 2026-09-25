module F (X : sig module N : sig val v : int end end) = struct
  let v = X.N.v
end
