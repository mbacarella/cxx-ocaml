type r = {a : int}
module F (X : sig type t end) = struct
  let v x = x.a
end
