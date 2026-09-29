module type S = sig
  module F : sig type t end
 end
let x = (module struct
    module F = struct type t end
  end : S)
