module F (X : sig module N : sig type t = {a : int} end end) = struct
  let v x = x.X.N.a
end
