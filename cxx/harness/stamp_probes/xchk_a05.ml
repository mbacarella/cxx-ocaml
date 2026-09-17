module type S = sig
  module F : sig type t end -> sig type t end
 end
let x = (module struct
    module F (_ : sig type t end) = struct type t end
  end : S)
