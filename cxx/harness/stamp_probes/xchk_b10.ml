module type S = sig
  module F : functor () -> sig end
 end
module X = (struct
    module F () = struct end
  end : S)
