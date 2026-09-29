let rec len = function [] -> 0 | _ :: t -> 1 + len t let x = len 3
