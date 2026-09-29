module F (X : sig type t = {a : int} end) = struct
  let v = function { X.a } -> a
end
