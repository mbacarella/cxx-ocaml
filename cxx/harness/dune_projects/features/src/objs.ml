class counter init = object (self)
  val mutable n = init
  method incr = n <- n + 1; self
  method get = n
end
class virtual shape name = object
  method name : string = name
  method virtual area : float
end
class square s = object inherit shape "square" method area = s *. s end
let total (l : #shape list) = List.fold_left (fun a s -> a +. s#area) 0. l
let o = object method hello = "hi" method twice x = 2 * x end
type color = [ `Red | `Green | `Rgb of int * int * int ]
let to_int : [< color ] -> int = function `Red -> 1 | `Green -> 2 | `Rgb (r, g, b) -> r + g + b
exception Found of int
let find p l = try List.iter (fun x -> if p x then raise (Found x)) l; None with Found x -> Some x
let r = ref 0
let () = for i = 1 to 10 do r := !r + i done
let fmt = Printf.sprintf "%d-%s-%.3f-%S" 1 "a" 2.5 "q"
let lazy_v = lazy (List.length [1; 2; 3])
let rec fib = function 0 | 1 -> 1 | n -> fib (n - 1) + fib (n - 2)
let arr = Array.init 5 (fun i -> i * i)
let () = assert (fib 5 = 8)
