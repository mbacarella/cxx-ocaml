module F (X : sig type t = {a : int} end) = struct
  let v (x : X.t) = x.a
end
