module type S = sig
  module F : functor () -> sig type t end
 end
let f () = (module struct
    module F () = struct type t end
  end : S)
