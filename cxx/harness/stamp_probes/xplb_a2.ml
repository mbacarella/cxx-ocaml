module F (X : sig type t = {a : int} end) = struct
  open X
  let v x = x.a
end
