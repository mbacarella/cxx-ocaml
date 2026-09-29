module F (X : sig type u module N : sig val v : u end end) = struct
  let v = X.N.v
end
