module F (X : sig type t = {a : int} end) = struct
  let v x = x.X.a
end
