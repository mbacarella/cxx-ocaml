let f x = (x :> [`A | `B])
let g (x : [`A]) = (x :> [`A | `B])
let h x = (x : [`A] :> [> `A])
