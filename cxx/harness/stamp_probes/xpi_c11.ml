module A = struct module type A_S = sig end type t = (module A_S) end
module type S = sig type t end
let f (type a) (module X : S with type t = a) = ()
module Aa : S with type t = (module A.A_S) = A
let _ = f (module A) let _ = f (module Aa)
let _ = f (module Aa : S with type t = (module A.A_S))
module Al = A module Ae = struct include Al end
let _ = f (module Ae : S with type t = (module A.A_S)) let _ = f (module Ae)
let _ = f (module Al : S with type t = (module A.A_S)) let _ = f (module Al)
