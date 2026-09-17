module type S = sig type t end
module M1 = struct type t = int end
module M2 = struct type t = bool end
let f (module M : S) = ()
class c = object method m = f (module M1) end
