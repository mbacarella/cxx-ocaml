module type S = sig
  module rec M : sig type u = int end
end
let x = 1
