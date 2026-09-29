module type S = sig
  module F : functor () -> sig end
 end
module X : S = struct
    module F () = struct end
  end
