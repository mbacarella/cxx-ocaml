module type S = sig
  module rec M : sig open Set.Make(Bool) type u = t end
end
let x = 1
