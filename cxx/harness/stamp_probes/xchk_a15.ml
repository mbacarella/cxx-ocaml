module type S = sig
  module F : functor () -> sig end
 end
let x = (module struct
    module F () = struct end
  end : S)
