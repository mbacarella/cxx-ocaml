open Gadt
let () =
  Printf.printf "%d %b\n" (eval (Add (Int 1, Int 2))) (fst (eval (Pair (Bool true, Int 0))));
  print_endline (String.concat "," (List.map string_of_int (Functors.IntSet.(elements (add 3 (add 1 empty))))));
  Printf.printf "%d %.1f %s\n" ((new Objs.counter 1)#incr#get) (Objs.total [new Objs.square 2.]) Objs.fmt
