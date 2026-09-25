module F (X : sig type t = {a : int} end) = struct
  type u = {a : string}
  let v x = x.a
end
